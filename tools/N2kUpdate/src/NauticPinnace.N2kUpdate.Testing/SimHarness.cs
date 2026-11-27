using System.Buffers.Binary;
using System.Security.Cryptography;
using NauticPinnace.N2kUpdate.Bus;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;

namespace NauticPinnace.N2kUpdate.Testing;

/// <summary>
/// A sender engine and a scripted display on one virtual bus, ready to run in
/// virtual time. The sender's adapter models a CANable (63 + 3 frames, 6.4).
/// </summary>
public sealed class SimHarness
{
    public const int CanableCapacity = 66;

    public SimHarness(DisplayScript? script = null, int senderCapacity = CanableCapacity, CanLogWriter? recorder = null,
                      BusLoadOptions? busLoad = null, int seed = 1)
    {
        Sim = new Simulation();
        Sim.Bus.Recorder = recorder;
        SenderLink = Sim.Bus.Attach("sender", senderCapacity);
        DisplayLink = Sim.Bus.Attach("display");
        Engine = new N2kEngine(SenderLink, Sim.Clock, new EngineOptions { UniqueNumber = 0x0ACE5, BusLoad = busLoad ?? new BusLoadOptions() }, new Random(seed));
        Engine.Log += m => Log.Add($"{Sim.NowUs / 1000,9} ms  {m}");
        Display = new ScriptedDisplay(DisplayLink, Sim.Clock, script);
        Sim.Nodes.Add(new EngineNode(Engine));
        Sim.Nodes.Add(Display);
    }

    public Simulation Sim { get; }
    public VirtualLink SenderLink { get; }
    public VirtualLink DisplayLink { get; }
    public N2kEngine Engine { get; }
    public ScriptedDisplay Display { get; }
    public List<string> Log { get; } = new();
    public List<UpdateProgress> Progress { get; } = new();

    /// <summary>Opens the sender, claims its address, and waits until it may send.</summary>
    public void Start()
    {
        Engine.Start();
        if (!Sim.RunUntil(() => Engine.Source.IsUsable, Sim.NowUs + 2_000_000))
            throw new InvalidOperationException("the sender could not claim an address");
    }

    public DiscoveryResult Discover()
    {
        var op = new DiscoveryOperation(Engine);
        Engine.Add(op);
        if (!Sim.RunUntil(() => op.IsFinished, Sim.NowUs + 10_000_000)) throw new TimeoutException("discovery did not finish");
        return op.Result!;
    }

    public DisplayDevice DiscoverOne()
    {
        var r = Discover();
        if (r.Displays.Count != 1) throw new InvalidOperationException($"expected one display, found {r.Displays.Count}");
        return r.Displays[0];
    }

    public UpdateJob StartUpdate(DisplayDevice dev, UpdateRequest request, UpdateOptions? options = null)
    {
        var opt = options ?? new UpdateOptions();
        var job = new UpdateJob(Engine, dev, request, opt, p => Progress.Add(p));
        Engine.SetBusLoad(opt.BusLoad);
        Engine.Add(job);
        return job;
    }

    /// <summary>Runs an update to the end (virtual time) and returns the report.</summary>
    public UpdateReport Update(DisplayDevice dev, UpdateRequest request, UpdateOptions? options = null, long limitUs = 3_600_000_000)
    {
        var job = StartUpdate(dev, request, options);
        if (!Sim.RunUntil(() => job.IsFinished, Sim.NowUs + limitUs))
            throw new TimeoutException("update did not finish; last log: " + string.Join(" | ", Log.TakeLast(5)));
        return job.Report!;
    }
}

/// <summary>
/// Test images. A real N2K-capable build may not exist yet; a test image is made from
/// a legacy firmware.bin by writing the descriptor at 0x120 and fixing the image
/// checksum and the appended SHA-256, so it passes every structural check. It does
/// NOT run on a display: never flash it.
/// </summary>
public static class TestImageFactory
{
    public static byte[] AddDescriptor(byte[] legacy, BoardId board, string version, byte flags = FwDescriptor.FlagOtaGuard)
    {
        var img = (byte[])legacy.Clone();
        var panel = Boards.Panel(board) ?? throw new ArgumentException("unknown board");
        var d = new FwDescriptor
        {
            Board = board, PanelWidth = (ushort)panel.W, PanelHeight = (ushort)panel.H,
            FwVersion = version, BoardName = Boards.BoardName(board)!, Flags = flags,
        }.Encode();
        d.CopyTo(img, FwDescriptor.Offset);
        FixChecksums(img);
        return img;
    }

    /// <summary>
    /// Recomputes the ESP image checksum (XOR of all segment data, seeded 0xEF, in the
    /// last byte before the 16-byte alignment) and the appended SHA-256 if present.
    /// </summary>
    public static void FixChecksums(byte[] img)
    {
        int segs = img[1];
        int off = 24;
        byte x = 0xEF;
        for (int i = 0; i < segs; i++)
        {
            int len = (int)BinaryPrimitives.ReadUInt32LittleEndian(img.AsSpan(off + 4));
            for (int k = 0; k < len; k++) x ^= img[off + 8 + k];
            off += 8 + len;
        }
        int cks = off + (15 - off % 16);
        img[cks] = x;
        if (img[23] == 1 && cks + 33 <= img.Length)
            SHA256.HashData(img.AsSpan(0, cks + 1)).CopyTo(img, cks + 1);
    }

    /// <summary>A small synthetic image with everything a sender checks (for fast tests).</summary>
    public static byte[] Synthetic(BoardId board, string version, int size = 100_000, int seed = 7, ImageId? id = null)
    {
        var rng = new Random(seed);
        var img = new byte[size];
        // Compressible but not trivial: text-like runs mixed with noise.
        for (int i = 0; i < size; i++) img[i] = (byte)(i % 97 < 60 ? "NauticPinnace firmware "[i % 23] : rng.Next(256));
        img[0] = 0xE9;
        BinaryPrimitives.WriteUInt32LittleEndian(img.AsSpan(0x20), FirmwareImage.AppDescMagic);
        var iid = id ?? new ImageId((ulong)rng.NextInt64());
        iid.Write(img.AsSpan(FirmwareImage.ImageIdOffset));
        var panel = Boards.Panel(board)!.Value;
        new FwDescriptor
        {
            Board = board, PanelWidth = (ushort)panel.W, PanelHeight = (ushort)panel.H,
            FwVersion = version, BoardName = Boards.BoardName(board)!,
        }.Encode().CopyTo(img, FwDescriptor.Offset);
        return img;
    }

    /// <summary>A gzip file of about the given size.</summary>
    public static byte[] SyntheticWebUi(int approxSize = 20_000, int seed = 3)
    {
        var rng = new Random(seed);
        var text = new System.Text.StringBuilder();
        while (text.Length < approxSize * 3)
            text.Append("<div class=\"card\">").Append(rng.Next()).Append("</div>\n");
        using var ms = new MemoryStream();
        using (var gz = new System.IO.Compression.GZipStream(ms, System.IO.Compression.CompressionLevel.SmallestSize, true))
        {
            var b = System.Text.Encoding.UTF8.GetBytes(text.ToString());
            gz.Write(b, 0, b.Length);
        }
        return ms.ToArray();
    }
}
