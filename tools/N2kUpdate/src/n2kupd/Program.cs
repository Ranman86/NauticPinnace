using System.Globalization;
using System.Text.Json;
using NauticPinnace.N2kUpdate;
using NauticPinnace.N2kUpdate.Bus;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.Hosting;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;
using NauticPinnace.N2kUpdate.Testing;

namespace NauticPinnace.N2kUpdate.Cli;

public static class Program
{
    private const string Usage = """
        n2kupd - updates over NMEA 2000 for NauticPinnace displays (docs/N2K-UPDATE.md)

        Commands:
          discover          --link <spec>
          identify          --link <spec> --uid <uid> [--seconds 10]
          update            --link <spec> [--uid <uid> | --all] (--release <folder|zip> | --firmware <bin> [--webui <gz|littlefs.bin>])
                            [--webui-version <v>] [--allow-version-mismatch] [--share 30] [--no-backoff] [--conservative]
                            [--raw] [--apply now|nowait|next|none] [--no-companion] [--firmware-only] [--webui-only]
                            [--record <file.canlog>]
          record            --out <dir> [--firmware <bin> | --legacy <bin>] [--board 7b] [--version 1.3.1] [--webui <gz>]
                            Runs sender and scripted display in virtual time and writes the frame logs of 14.3.2.
          sim-display       (--listen <port> | --stdio) [--board 7b] [--version 1.3.0] [--uid <uid>] [--address 42] [--decline]
                            The scripted display as an slcan adapter: a TCP server, or on stdin/stdout.
          make-test-image   (--legacy <firmware.bin> | --synthetic <bytes>) --board 4|7b|5b --version <v> --out <bin>
                            A descriptor on a legacy image (checksums fixed), or a small synthetic image.
                            For tests only, never flash it.

        Link specs:
          slcan:COM7 | slcan:/dev/ttyACM0 | slcan:auto    a CANable with slcan firmware (auto: by USB id 16D0:117E)
          tcp:<host>:<port>                               slcan text over TCP, we connect (C++ n2kupd_hostsim, 14.3.3)
          tcp-listen:<port>                               slcan text over TCP, we wait for the peer to connect
          stdio:<program> [args]                          slcan text over the stdin/stdout of a child process
          sim                                             the scripted display in virtual time (no hardware)
        """;

    public static int Main(string[] args)
    {
        // Numbers print with a decimal point on every Windows (owner's decision;
        // the CLI speaks English only). The default culture, not the current
        // thread's: progress is reported from the engine's thread.
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
        if (args.Length == 0 || args[0] is "-h" or "--help" or "help") { Console.WriteLine(Usage); return 0; }
        var a = new Args(args.Skip(1));
        try
        {
            return args[0] switch
            {
                "discover" => Discover(a),
                "identify" => Identify(a),
                "update" => Update(a),
                "record" => Recorder.Run(a),
                "sim-display" => SimDisplay(a),
                "make-test-image" => MakeTestImage(a),
                _ => Fail($"unknown command {args[0]}\n\n{Usage}"),
            };
        }
        catch (ArgumentException ex) { return Fail(ex.Message); }
        catch (IOException ex) { return Fail(ex.Message); }
        catch (InvalidDataException ex) { return Fail(ex.Message); }
    }

    internal static int Fail(string msg)
    {
        Console.Error.WriteLine("error: " + msg);
        return 2;
    }

    // ── Links and the client ──────────────────────────────────────────────────
    private static ICanFrameLink OpenLinkSpec(string spec)
    {
        int colon = spec.IndexOf(':');
        string kind = colon < 0 ? spec : spec[..colon], rest = colon < 0 ? "" : spec[(colon + 1)..];
        switch (kind)
        {
            case "slcan":
                if (rest is "" or "auto")
                {
                    var link = new SlcanFrameLink(() => CanablePorts.Find());
                    return link;
                }
                // A port that was not identified as a CANable could be a display's own
                // USB port: do not toggle DTR/RTS there (it would reset an ESP32-S3).
                bool isCanable = CanablePorts.FindAll().Contains(rest, StringComparer.OrdinalIgnoreCase);
                return new SlcanFrameLink(rest) { AssertDtrRts = isCanable };
            case "tcp":
                int c2 = rest.LastIndexOf(':');
                if (c2 < 0) throw new ArgumentException("tcp:<host>:<port>");
                return SlcanFrameLink.ForTcp(rest[..c2], Args.WholeNumber(rest[(c2 + 1)..], "tcp port"));
            case "tcp-listen":
                return SlcanFrameLink.ForTcpListen(Args.WholeNumber(rest, "tcp-listen port"));
            case "stdio":
                // stdio:<program> [args], or stdio:"<program with spaces>" [args]
                string cmd = rest.Trim();
                string prog, progArgs;
                if (cmd.StartsWith('"'))
                {
                    int q = cmd.IndexOf('"', 1);
                    if (q < 0) throw new ArgumentException("stdio: unbalanced quote");
                    prog = cmd[1..q];
                    progArgs = cmd[(q + 1)..].Trim();
                }
                else
                {
                    int sp = cmd.IndexOf(' ');
                    prog = sp < 0 ? cmd : cmd[..sp];
                    progArgs = sp < 0 ? "" : cmd[(sp + 1)..];
                }
                return SlcanFrameLink.ForProcess(prog, progArgs);
            default:
                throw new ArgumentException($"unknown link spec {spec}");
        }
    }

    private static IHostWaiter Waiter()
    {
        if (OperatingSystem.IsWindows()) return new WindowsHighResWaiter();
        return DefaultWaiter.Instance;
    }

    private static N2kUpdateClient Client(Args a)
    {
        string spec = a.Get("link") ?? throw new ArgumentException("--link is required");
        ICanFrameLink link = OpenLinkSpec(spec);
        if (a.Get("record") is { } rec) link = new RecordingFrameLink(link, new CanLogWriter(rec, SystemClock.Instance.NowUs));
        var settings = UpdaterSettings.Load();
        var opt = new EngineOptions
        {
            // The CLI's NAME differs from the window updater's in bit 0, so both may share a bus.
            UniqueNumber = a.GetInt("unique") ?? (settings.UniqueNumber ^ 1),
            PreferredAddress = a.GetInt("address") ?? 230,
            ProductInfo = new N2k.ProductInfo { ProductCode = 2046, ModelId = "NauticPinnace Updater", SoftwareVersion = "1.0.0 cli" },
        };
        var client = new N2kUpdateClient(link, opt, Waiter());
        client.Log += m => Console.WriteLine($"  {DateTime.Now:HH:mm:ss}  {m}");
        return client;
    }

    private static int Discover(Args a)
    {
        if (a.Get("link") == "sim") return Fail("use 'record' or 'update --link sim' for the virtual display");
        using var client = Client(a);
        client.StartAsync().GetAwaiter().GetResult();
        Console.WriteLine($"source address {client.SourceAddress}");
        var r = client.DiscoverAsync().GetAwaiter().GetResult();
        PrintDiscovery(r);
        return 0;
    }

    internal static void PrintDiscovery(DiscoveryResult r)
    {
        Console.WriteLine($"{"addr",4}  {"board",-6} {"label",-16} {"version",-15} {"ota",-14} {"accept",-6} {"web UI",-15} uid");
        foreach (var d in r.Displays)
        {
            var i = d.Info;
            Console.WriteLine($"{d.Address,4}  {Boards.DisplayName(i.Board),-6} {i.Label,-16} {i.RunningVersion,-15} {i.OtaState,-14} " +
                              $"{((i.Flags & StatusFlags.AcceptUpdates) != 0 ? "yes" : "no"),-6} {i.WebUiVersion,-15} {i.Uid.LastSixHex}");
        }
        foreach (var l in r.Legacy)
            Console.WriteLine($"{l.Address,4}  NauticPinnace, firmware {l.SoftwareVersion}: no update over NMEA 2000 yet; update it once over WiFi or USB");
        if (r.Displays.Count == 0 && r.Legacy.Count == 0) Console.WriteLine("no display found");
    }

    /// <summary>
    /// The displays a --uid names: the full id ("02:00:00:00:00:01") or the last six
    /// hex digits the discovery table prints ("000001"). The table only shows the
    /// short form, so an update that accepted only the full id could not be started
    /// from what the tool had just printed (bench test, 2026-10-13).
    /// </summary>
    internal static List<DisplayDevice> MatchUid(string? text, IEnumerable<DisplayDevice> displays)
    {
        if (string.IsNullOrWhiteSpace(text)) return new List<DisplayDevice>();
        if (UniqueId.TryParse(text, out var uid)) return displays.Where(d => d.Uid == uid).ToList();
        string hex = new string(text.Where(Uri.IsHexDigit).ToArray()).ToUpperInvariant();
        if (hex.Length != 6) return new List<DisplayDevice>();
        return displays.Where(d => d.Uid.LastSixHex == hex).ToList();
    }

    private static int Identify(Args a)
    {
        if (string.IsNullOrWhiteSpace(a.Get("uid"))) return Fail("--uid is required");
        using var client = Client(a);
        client.StartAsync().GetAwaiter().GetResult();
        var r = client.DiscoverAsync().GetAwaiter().GetResult();
        var hits = MatchUid(a.Get("uid"), r.Displays);
        if (hits.Count > 1) return Fail($"--uid {a.Get("uid")} matches several displays: give the full id");
        var d = hits.FirstOrDefault();
        if (d is null) return Fail($"display {a.Get("uid")} not found");
        client.IdentifyAsync(d, a.GetInt("seconds") ?? 10).GetAwaiter().GetResult();
        Console.WriteLine($"IDENTIFY sent to {d}");
        return 0;
    }

    internal static UpdateOptions Options(Args a, string? cacheDir) => new()
    {
        BusLoad = new BusLoadOptions
        {
            BusShare = (a.GetNumber("share") ?? 30) / 100.0,
            AdaptiveBackoff = !a.Has("no-backoff"),
            ConservativeFrameBits = a.Has("conservative"),
        },
        Compression = a.Has("raw") ? CompressionMode.Off : CompressionMode.Auto,
        ApplyMode = (a.Get("apply") ?? "now") switch
        {
            "now" => ApplyMode.Now,
            "nowait" => ApplyMode.NowNoWait,
            "next" => ApplyMode.NextRestart,
            "none" => ApplyMode.None,
            var x => throw new ArgumentException($"--apply {x}: now|nowait|next|none"),
        },
        IncludeCompanionWebUi = !a.Has("no-companion"),
        AllowWebUiVersionMismatch = a.Has("allow-version-mismatch"),
        InstallFirmware = !a.Has("webui-only"),
        InstallWebUi = !a.Has("firmware-only"),
        SenderName = "PC Updater",
        SenderKind = SenderKind.StandaloneUpdater,
        CacheDirectory = cacheDir,
    };

    /// <summary>The request for one display: from a release (by board) or from single files.</summary>
    internal static UpdateRequest? RequestFor(Args a, ReleasePackage? release, DisplayDevice d, out string? why)
    {
        why = null;
        if (release is not null)
        {
            var bp = release.For(d.Board) ?? release.For(BoardId.Any);
            if (bp is null) { why = $"the release has nothing for a {Boards.DisplayName(d.Board)}"; return null; }
            return new UpdateRequest { Firmware = bp.Firmware, WebUi = bp.WebUi, WebUiVersion = a.Get("webui-version") ?? bp.WebUiVersion };
        }
        var (fw, web) = ReleasePackage.LoadFiles(a.Get("firmware"), a.Get("webui"));
        return new UpdateRequest { Firmware = fw, WebUi = web, WebUiVersion = a.Get("webui-version") ?? fw?.Descriptor?.FwVersion };
    }

    private static int Update(Args a)
    {
        if (a.Get("link") == "sim") return Recorder.SimUpdate(a);
        ReleasePackage? release = a.Get("release") is { } rp ? ReleasePackage.Load(rp) : null;
        if (release is not null)
            foreach (var n in release.Notes) Console.WriteLine("  note: " + n);

        using var client = Client(a);
        client.StartAsync().GetAwaiter().GetResult();
        var found = client.DiscoverAsync().GetAwaiter().GetResult();
        PrintDiscovery(found);
        List<DisplayDevice> targets;
        if (a.Has("all")) targets = found.Displays.ToList();
        else if (!string.IsNullOrWhiteSpace(a.Get("uid")))
        {
            targets = MatchUid(a.Get("uid"), found.Displays);
            if (targets.Count > 1) return Fail($"--uid {a.Get("uid")} matches several displays: give the full id");
        }
        else if (found.Displays.Count == 1) targets = found.Displays.ToList();
        else return Fail("several displays: choose one with --uid, or --all");
        if (targets.Count == 0) return Fail("no matching display");

        using var cts = new CancellationTokenSource();
        bool mustStay = false;
        DateTime warned = DateTime.MinValue;
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            if (mustStay && DateTime.UtcNow - warned > TimeSpan.FromSeconds(5))
            {
                // H5: closing now makes the display roll back (12.7).
                Console.WriteLine("\n  The display is checking the new firmware and needs this program's polls. Press Ctrl+C again");
                Console.WriteLine("  within 5 s to stop anyway: the display will then roll back to its previous firmware.");
                warned = DateTime.UtcNow;
                return;
            }
            cts.Cancel();
        };

        int bad = 0;
        IDisposable? awake = OperatingSystem.IsWindows() ? WindowsHighResWaiter.KeepAwake() : null;
        try
        {
            foreach (var d in targets)
            {
                var req = RequestFor(a, release, d, out string? why);
                if (req is null) { Console.WriteLine($"{d}: {why}"); bad++; continue; }
                Console.WriteLine($"\n{d}: installing" + (req.Firmware is not null ? $" firmware {req.Firmware.Version}" : "") +
                                  (req.WebUi is not null ? $" web UI ({req.WebUi.Size} B)" : ""));
                var progress = new ConsoleProgress(p => mustStay = p.MustStayConnected);
                var rep = client.UpdateAsync(d, req, Options(a, UpdaterSettings.CacheFolder), progress, cts.Token).GetAwaiter().GetResult();
                progress.Done();
                Console.WriteLine($"  firmware: {rep.Firmware?.ToString() ?? "-"}");
                Console.WriteLine($"  web UI:   {rep.WebUi?.ToString() ?? "-"}");
                if (!rep.IsGood) bad++;
                if (cts.IsCancellationRequested) break;
            }
        }
        finally { awake?.Dispose(); }
        return bad == 0 ? 0 : 1;
    }

    private sealed class ConsoleProgress(Action<UpdateProgress> also) : IProgress<UpdateProgress>
    {
        private string _last = "";
        public void Report(UpdateProgress p)
        {
            also(p);
            string line = p.Phase switch
            {
                SenderPhase.WaitingForConfirmation => $"waiting for confirmation on the display ({p.DialogSecondsLeft} s)",
                SenderPhase.Transferring => $"{p.Target}: {p.Fraction:P0}" + (p.EtaSeconds is { } e ? $", about {e / 60:F1} min left" : "") +
                                            $", own load {p.OwnLoad:P0}, bus {p.TotalLoad:P0}, share {p.EffectiveShare:P0}",
                SenderPhase.WaitingForValidation => "do not close: the display is checking the new firmware (about 1-2 min)" +
                                                    (p.VerifySecondsLeft is { } v ? $", {v} s left" : ""),
                _ => $"{p.Target}: {p.Phase}" + (p.Message.Length > 0 ? " - " + p.Message : ""),
            };
            if (line == _last) return;
            _last = line;
            Console.Write("\r  " + line.PadRight(100)[..100]);
        }
        public void Done() => Console.WriteLine();
    }

    // ── Scripted display behind TCP ───────────────────────────────────────────
    private static int SimDisplay(Args a)
    {
        var clock = SystemClock.Instance;
        bool stdio = a.Has("stdio");
        // With --stdio, stdout carries slcan: everything for people goes to stderr.
        TextWriter say = stdio ? Console.Error : Console.Out;
        SlcanServerLink link = stdio
            ? SlcanServerLink.ForStdio(Console.OpenStandardInput(), Console.OpenStandardOutput(), clock)
            : new SlcanServerLink(a.GetInt("listen") ?? throw new ArgumentException("--listen <port> or --stdio"), clock);
        link.Log += m => say.WriteLine($"  {DateTime.Now:HH:mm:ss}  {m}");
        link.Open();
        var script = new DisplayScript { User = a.Has("decline") ? DisplayScript.UserAction.Decline : DisplayScript.UserAction.Accept };
        var d = new ScriptedDisplay(link, clock, script)
        {
            Board = ParseBoard(a.Get("board") ?? "7b"),
            Address = a.GetInt("address") ?? 0x2A,
        };
        if (UniqueId.TryParse(a.Get("uid"), out var uid)) d.Uid = uid;
        d.SetRunning(a.Get("version") ?? "1.3.0", new ImageId(0x1122334455667788));
        using var host = new RealTimeDisplayHost(d, clock);
        say.WriteLine($"scripted {Boards.DisplayName(d.Board)} display {d.Uid} at address {d.Address}, slcan " +
                      (stdio ? "over stdin/stdout" : $"over TCP on 127.0.0.1:{link.Port}") + ". Ctrl+C ends.");
        var done = new ManualResetEventSlim();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; done.Set(); };
        if (stdio) link.LinkEvent += (k, _, _) => { if (k == LinkEventKind.Closed) done.Set(); };
        int shown = 0;
        while (!done.Wait(500))
        {
            host.With(x =>
            {
                for (; shown < x.Events.Count; shown++) say.WriteLine("  display " + x.Events[shown]);
            });
        }
        link.Close();
        return 0;
    }

    internal static BoardId ParseBoard(string s) => s.ToLowerInvariant() switch
    {
        "4" or "4inch" or "4-inch" => BoardId.Inch4,
        "7b" => BoardId.B7,
        "5b" => BoardId.B5,
        _ => throw new ArgumentException($"board {s}: 4, 7b or 5b"),
    };

    private static int MakeTestImage(Args a)
    {
        string outPath = a.Get("out") ?? throw new ArgumentException("--out <bin>");
        var board = ParseBoard(a.Get("board") ?? throw new ArgumentException("--board 4|7b|5b"));
        string version = a.Get("version") ?? throw new ArgumentException("--version <v>");
        byte[] img = a.GetInt("synthetic") is int size
            ? TestImageFactory.Synthetic(board, version, size)
            : TestImageFactory.AddDescriptor(File.ReadAllBytes(a.Get("legacy") ?? throw new ArgumentException("--legacy <firmware.bin> or --synthetic <bytes>")), board, version);
        File.WriteAllBytes(outPath, img);
        var fw = FirmwareImage.FromBytes(img);
        Console.WriteLine($"{outPath}: {fw.Size} B, {Boards.DisplayName(fw.Board)} {fw.Version}, image id {fw.ImageId}, " +
                          $"valid: {fw.IsValid}. TEST IMAGE: never flash it.");
        return 0;
    }
}

/// <summary>Minimal "--key value" / "--flag" parser.</summary>
internal sealed class Args
{
    private readonly Dictionary<string, string?> _v = new(StringComparer.OrdinalIgnoreCase);

    public Args(IEnumerable<string> args)
    {
        var list = args.ToList();
        for (int i = 0; i < list.Count; i++)
        {
            if (!list[i].StartsWith("--", StringComparison.Ordinal)) throw new ArgumentException($"unexpected argument {list[i]}");
            string key = list[i][2..];
            string? val = i + 1 < list.Count && !list[i + 1].StartsWith("--", StringComparison.Ordinal) ? list[++i] : null;
            _v[key] = val;
        }
    }

    public string? Get(string key) => _v.TryGetValue(key, out var v) ? v : null;
    public bool Has(string key) => _v.ContainsKey(key);
    public int? GetInt(string key) => Get(key) is { } s ? WholeNumber(s, "--" + key) : null;

    /// <summary>A number that may have a fraction; a comma counts as the decimal point, like '.'.</summary>
    public double? GetNumber(string key)
    {
        if (Get(key) is not { } s) return null;
        if (double.TryParse(s.Trim().Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out double v) && double.IsFinite(v))
            return v;
        throw new ArgumentException($"--{key} {s}: a number expected");
    }

    /// <summary>
    /// A whole number, decimal or 0x hex. Bad input is an ArgumentException, which
    /// Main prints as "error: ...", instead of a FormatException with a stack trace.
    /// </summary>
    public static int WholeNumber(string s, string what)
    {
        int n;
        bool ok = s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? int.TryParse(s.AsSpan(2), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out n)
            : int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out n);
        return ok ? n : throw new ArgumentException($"{what} {s}: a whole number expected");
    }
}

internal static class Json
{
    public static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };
}
