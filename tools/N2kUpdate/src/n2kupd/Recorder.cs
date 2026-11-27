using System.Text.Json;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;
using NauticPinnace.N2kUpdate.Testing;

namespace NauticPinnace.N2kUpdate.Cli;

/// <summary>
/// The frame recorder of the cross test (14.3.2): the sender runs against the
/// scripted display on a virtual bus in virtual time, and every frame on the wire
/// goes into a frame log. Each log comes with a .meta.json (addresses, images,
/// timing of the scripted display, outcome) and the display's event timeline.
/// </summary>
internal static class Recorder
{
    private sealed record Scenario(string Name, string Description, bool Firmware, bool WebUi, bool Deflate, ApplyMode Apply, bool Loss, string DisplayVersion);

    private static readonly Scenario[] Scenarios =
    [
        new("fw_deflate", "firmware, deflate, APPLY now, polled until the display validates it", true, false, true, ApplyMode.Now, false, "1.3.0"),
        new("webui", "web UI alone with its own confirmation dialog", false, true, false, ApplyMode.Now, false, "*"),
        new("release_fw_webui", "firmware (deflate) with the companion web UI: one dialog, then the web UI without a second one", true, true, true, ApplyMode.Now, false, "1.3.0"),
        new("fw_raw_loss", "firmware, raw, frames lost on the wire (bitmap recovery), transfer and verify only", true, false, false, ApplyMode.None, true, "1.3.0"),
    ];

    public static int Run(Args a)
    {
        string outDir = a.Get("out") ?? throw new ArgumentException("--out <dir>");
        Directory.CreateDirectory(outDir);
        Directory.CreateDirectory(Path.Combine(outDir, "images"));

        var (fw, fwOrigin) = LoadFirmware(a);
        var (web, webOrigin) = LoadWebUi(a);
        string fwFile = $"images/firmware_{Boards.FolderName(fw.Board)}_{fw.Version}.bin";
        File.WriteAllBytes(Path.Combine(outDir, fwFile), fw.Bytes);
        File.WriteAllBytes(Path.Combine(outDir, "images/index.html.gz"), web.Bytes);
        Console.WriteLine($"firmware {fw.Version} for {Boards.DisplayName(fw.Board)}, {fw.Size} B, image id {fw.ImageId} ({fwOrigin})");
        Console.WriteLine($"web UI {web.Size} B, id {web.WebUiId} ({webOrigin})");

        var only = a.Get("only");
        int bad = 0;
        foreach (var sc in Scenarios.Where(s => only is null || s.Name == only))
        {
            var meta = RunScenario(sc, outDir, fw, fwFile, web, "images/index.html.gz");
            Console.WriteLine($"{sc.Name}: {meta["frames"]} frames, {meta["durationMs"]} ms virtual, firmware {meta["firmwareOutcome"]}, web UI {meta["webUiOutcome"]}");
            if (!(bool)meta["good"]!) bad++;
        }
        return bad == 0 ? 0 : 1;
    }

    private static (FirmwareImage, string) LoadFirmware(Args a)
    {
        if (a.Get("firmware") is { } f)
        {
            var fw = FirmwareImage.Load(f);
            if (!fw.IsValid) throw new InvalidDataException($"{f}: " + string.Join("; ", fw.Problems));
            return (fw, f);
        }
        string? legacy = a.Get("legacy") ?? FindRepoFile("release/NauticPinnace-v1.2.0/7b/firmware.bin");
        if (legacy is null) throw new ArgumentException("--firmware <N2K-capable firmware.bin> or --legacy <firmware.bin> is required");
        var legacyImg = FirmwareImage.Load(legacy);
        var board = a.Get("board") is { } b ? Program.ParseBoard(b) : legacyImg.LegacyBoard ?? BoardId.B7;
        string version = a.Get("version") ?? "1.3.1";
        var img = FirmwareImage.FromBytes(TestImageFactory.AddDescriptor(legacyImg.Bytes, board, version), legacy + " + test descriptor");
        return (img, $"test image: {Path.GetFileName(legacy)} of {Boards.DisplayName(board)} with a {version} descriptor, checksums fixed");
    }

    private static (WebUiImage, string) LoadWebUi(Args a)
    {
        string? p = a.Get("webui") ?? FindRepoFile("data/index.html.gz");
        if (p is not null) return (WebUiImage.Load(p), p);
        return (WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi(48_000)), "synthetic");
    }

    /// <summary>A file relative to the repository root, found by walking up from the program.</summary>
    private static string? FindRepoFile(string rel)
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
            for (var d = new DirectoryInfo(start); d is not null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "platformio.ini")))
                {
                    string p = Path.Combine(d.FullName, rel);
                    return File.Exists(p) ? p : null;
                }
        return null;
    }

    private static Dictionary<string, object?> RunScenario(Scenario sc, string outDir, FirmwareImage fw, string fwFile, WebUiImage web, string webFile)
    {
        string log = Path.Combine(outDir, sc.Name + ".canlog");
        var lost = new List<string>();
        var script = new DisplayScript();
        using var writer = new CanLogWriter(log, 0);
        var h = new SimHarness(script, recorder: writer);
        h.Display.Board = fw.Board;
        string displayVersion = sc.DisplayVersion == "*" ? fw.Version : sc.DisplayVersion;
        h.Display.SetRunning(displayVersion, new ImageId(0x1122334455667788));
        if (sc.Loss)
        {
            // Every 40th DATA message loses its frame 9, every 97th its frame 0: the
            // display drops those messages and asks again through the bitmap.
            int msg = -1;
            h.Sim.Bus.DropOnWire = (f, from) =>
            {
                if (from != h.SenderLink || CanId.Decode(f.Id).Priority != 7) return false;
                int idx = f.Data[0] & 0x1F;
                if (idx == 0) msg++;
                bool drop = (msg % 40 == 39 && idx == 9) || (msg % 97 == 50 && idx == 0);
                if (drop) lost.Add($"{h.Sim.NowUs / 1000} {f.Id:X8} 8 {Convert.ToHexString(f.Data)}");
                return drop;
            };
        }
        h.Start();
        var dev = h.DiscoverOne();
        var req = new UpdateRequest
        {
            Firmware = sc.Firmware ? fw : null,
            WebUi = sc.WebUi ? web : null,
            WebUiVersion = fw.Version,
        };
        var opt = new UpdateOptions
        {
            Compression = sc.Deflate ? CompressionMode.Auto : CompressionMode.Off,
            ApplyMode = sc.Apply,
            InstallFirmware = sc.Firmware,
            InstallWebUi = sc.WebUi,
        };
        var job = h.StartUpdate(dev, req, opt);
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 3_600_000_000);
        h.Sim.RunFor(2_000_000);   // let the last frames settle
        writer.Flush();
        var rep = job.Report;

        var images = new List<object>();
        if (sc.Firmware)
        {
            var t = TransferImage.ForFirmware(fw, sc.Deflate);
            images.Add(new
            {
                target = "firmware", file = fwFile, board = (int)fw.Board, version = fw.Version, imageSize = t.ImageSize,
                sha256 = Convert.ToHexString(fw.Sha256).ToLowerInvariant(), imageId = fw.ImageId.ToString(),
                encoding = t.Encoding.ToString().ToLowerInvariant(), streamSize = t.StreamSize, streamCrc32 = $"0x{t.StreamCrc32:X8}",
                deflateEncoder = sc.Deflate ? DeflateCache.EncoderId : null,
            });
        }
        if (sc.WebUi)
            images.Add(new
            {
                target = "webui", file = webFile, version = fw.Version, imageSize = web.Size,
                sha256 = Convert.ToHexString(web.Sha256).ToLowerInvariant(), webUiId = web.WebUiId.ToString(),
                encoding = "raw", streamSize = web.Size, streamCrc32 = $"0x{web.Crc32:X8}",
            });

        var meta = new Dictionary<string, object?>
        {
            ["scenario"] = sc.Name,
            ["description"] = sc.Description,
            ["spec"] = "docs/N2K-UPDATE.md, protocol version 1 revision 2",
            ["format"] = "one frame per line: t_ms id29 dlc hex. t_ms = integer milliseconds of virtual time at the END of the frame on the wire; " +
                         "id29 = 8 hex digits; dlc = decimal; hex = data bytes without separators. Both directions in one file; " +
                         "the source address (id29 & 0xFF) tells sender and display apart. Line order = bus order.",
            ["frames"] = writer.Lines,
            ["durationMs"] = h.Sim.NowUs / 1000,
            ["sender"] = new { address = h.Engine.Source.Address, name = h.Engine.Options.Name.ToString(), uniqueNumber = h.Engine.Options.UniqueNumber, adapterFrames = SimHarness.CanableCapacity, busShare = 0.30 },
            ["display"] = new
            {
                address = 0x2A, uid = h.Display.Uid.ToString(), board = (int)h.Display.Board, runningVersion = displayVersion,
                runningImageId = "1122334455667788", runningSlot = 0, webUiVersion = "1.3.0", webUiId = "37558A1A25D09B4D",
                caps = "0x3F", maxFirmwareSize = "0x4EF000", maxWebUiSize = "0x20000",
                timing = new
                {
                    userDecisionAfterDialogMs = script.UserDelayUs / 1000, prepareMs = script.PrepareUs / 1000,
                    processingPerWindowMs = script.ProcessingUs / 1000, announcedHoldMs = script.HoldMs, verifyMs = script.VerifyUs / 1000,
                    applyMs = script.ApplyUs / 1000, restartCountdownMs = 10_000, bootUntilSetupDoneMs = script.BootUs / 1000,
                    markValidAfterSetupDoneMs = 60_000, statusKeepaliveMs = 1000, resultRepeatMs = 5000,
                },
            },
            ["images"] = images,
            ["firmwareOutcome"] = rep?.Firmware?.ToString() ?? "-",
            ["webUiOutcome"] = rep?.WebUi?.ToString() ?? "-",
            ["good"] = rep?.IsGood ?? false,
            ["framesLostOnWire"] = lost,
            ["displayEvents"] = h.Display.Events,
            ["senderLog"] = h.Log,
        };
        File.WriteAllText(Path.Combine(outDir, sc.Name + ".meta.json"), JsonSerializer.Serialize(meta, Json.Indented));
        return meta;
    }

    /// <summary><c>update --link sim</c>: the same as a real update, against the scripted display in virtual time.</summary>
    public static int SimUpdate(Args a)
    {
        ReleasePackage? release = a.Get("release") is { } rp ? ReleasePackage.Load(rp) : null;
        CanLogWriter? rec = a.Get("record") is { } path ? new CanLogWriter(path, 0) : null;
        var h = new SimHarness(recorder: rec);
        try
        {
            var probe = Program.RequestFor(a, release, new DisplayDevice(0x2A, new InfoMsg { Board = BoardId.B7 }), out _);
            h.Display.Board = probe?.Firmware?.Board is { } b and not BoardId.Any ? b : BoardId.B7;
            h.Display.SetRunning(a.Get("display-version") ?? "1.3.0", new ImageId(0x1122334455667788));
            h.Start();
            var found = h.Discover();
            Program.PrintDiscovery(found);
            var dev = found.Displays[0];
            var req = Program.RequestFor(a, release, dev, out string? why) ?? throw new ArgumentException(why);
            var rep = h.Update(dev, req, Program.Options(a, null));
            foreach (var l in h.Log) Console.WriteLine("  " + l);
            Console.WriteLine($"firmware: {rep.Firmware?.ToString() ?? "-"}");
            Console.WriteLine($"web UI:   {rep.WebUi?.ToString() ?? "-"}");
            Console.WriteLine($"virtual time {h.Sim.NowUs / 1e6:F1} s, {h.Sim.Bus.FramesOnWire} frames on the bus");
            return rep.IsGood ? 0 : 1;
        }
        finally { rec?.Dispose(); }
    }
}
