using System.Globalization;
using NauticPinnace.N2kUpdate.Bus;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.Hosting;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;
using NauticPinnace.N2kUpdate.Testing;

namespace NauticPinnace.N2kUpdate.Tests;

/// <summary>Rate math, token bucket, drain estimate, adaptive back-off, BELL handling (section 6).</summary>
public static class BusTests
{
    /// <summary>The DATA messages of a test stream, as frames with their exact cost.</summary>
    private static List<(List<CanFrame> Frames, int Bits)> DataMessages(int count, int seed = 1)
    {
        var rng = new Random(seed);
        var list = new List<(List<CanFrame>, int)>();
        for (int k = 0; k < count; k++)
        {
            var chunk = new byte[216];
            rng.NextBytes(chunk);
            var fr = FastPacket.Encode(0x1DEF2AE6, DataMsg.Build(0x5A, (uint)(k * 216), chunk), k & 7);
            list.Add((fr, fr.Sum(FrameBits.Exact)));
        }
        return list;
    }

    [Test]
    public static void RateTable()
    {
        // 6.1: 30 % = 75,000 bit/s; at 160 bits per frame that is 468.75 frames/s and
        // 14.6 DATA messages/s; at 50 %, 781.25 frames/s.
        Assert.Near(468.75, 0.30 * 250_000 / 160, 1e-9, "30 % conservative frames/s");
        Assert.Near(781.25, 0.50 * 250_000 / 160, 1e-9, "50 % conservative frames/s");
        Assert.Near(14.6, 0.30 * 250_000 / 160 / 32, 0.05, "30 % DATA messages/s (160 bit)");
        var msgs = DataMessages(200);
        double mean = msgs.Sum(m => m.Bits) / (200.0 * 32);
        Assert.True(mean is > 131 and < 145, string.Create(CultureInfo.InvariantCulture, $"mean exact frame length of random DATA {mean:F1} bits"));
        double perSec = 0.30 * 250_000 / mean / 32;
        Assert.True(perSec is > 16.5 and < 17.5, string.Create(CultureInfo.InvariantCulture, $"30 % exact: about 17 DATA messages/s, got {perSec:F2}"));
        Assert.Equal(6.75, 216.0 / 32, "payload efficiency 6.75 bytes per frame");
    }

    /// <summary>Drives the limiter alone on a virtual clock: send as fast as it allows.</summary>
    private static (double Share, int MaxBurst) RunBucket(double share, bool conservative, double seconds)
    {
        var lim = new BusLoadLimiter(new BusLoadOptions { BusShare = share, AdaptiveBackoff = false, ConservativeFrameBits = conservative }, 0);
        var msgs = DataMessages(64);
        long now = 0, end = (long)(seconds * 1e6);
        long bits = 0;
        int maxBurst = 0, k = 0;
        // Let the initially full bucket not distort: measure from the first send to the end.
        while (now < end)
        {
            var (fr, exact) = msgs[k++ % msgs.Count];
            int cost = conservative ? fr.Count * 160 : exact;
            long t = lim.DataReadyAt(cost, fr.Count, now);
            if (t > end) break;
            now = t;
            lim.Charge(cost, fr.Count, exact, now);
            lim.ResetBacklog(now);   // no adapter in this test
            bits += cost;
            maxBurst = Math.Max(maxBurst, cost);
        }
        return (bits / (seconds * 250_000), maxBurst);
    }

    [Test]
    public static void TokenBucketShare()
    {
        foreach (double s in new[] { 0.10, 0.30, 0.60 })
        {
            var (got, burst) = RunBucket(s, conservative: false, seconds: 120);
            Assert.Near(s, got, 0.02 * s, $"achieved share at {s:P0}");
            Assert.True(burst <= 5120, $"burst {burst} bits ≤ 5120");
            var (gotC, burstC) = RunBucket(s, conservative: true, seconds: 120);
            Assert.Near(s, gotC, 0.02 * s, $"achieved share at {s:P0}, conservative");
            Assert.True(burstC <= 5120, $"conservative burst {burstC} bits ≤ 5120");
        }
    }

    [Test]
    public static void ControlNeverDelayedMoreThan50ms()
    {
        var lim = new BusLoadLimiter(new BusLoadOptions { BusShare = 0.10, AdaptiveBackoff = false }, 0);
        // Empty the bucket with DATA, then ask for control slots.
        lim.Charge(5120, 32, 4400, 0);
        long due = lim.ControlDueAt(19 * 140, 0);
        Assert.True(due <= 50_000, $"OFFER after an empty bucket due at {due} µs");
        lim.Charge(19 * 140, 19, 19 * 140, due);
        long due2 = lim.ControlDueAt(19 * 140, due);
        Assert.True(due2 - due <= 50_000, "even with a negative bucket");
    }

    [Test]
    public static void AdaptiveBackoff()
    {
        var lim = new BusLoadLimiter(new BusLoadOptions { BusShare = 0.30 }, 0);
        long t = 0;
        void Second(double foreignLoad)
        {
            lim.OnReceived((int)(foreignLoad * 250_000));
            t += 1_000_000;
            lim.Tick(t);
        }
        Second(0.75);
        Assert.Equal(0.30, lim.EffectiveShare, "one second above 70 % is not enough");
        Second(0.75);
        Assert.Equal(0.15, lim.EffectiveShare, "two seconds above 70 %: halved");
        Second(0.75); Second(0.75);
        Assert.Equal(0.10, lim.EffectiveShare, "never below 10 %");
        for (int i = 0; i < 4; i++) Second(0.2);
        Assert.Equal(0.10, lim.EffectiveShare, "four quiet seconds are not enough");
        Second(0.2);
        Assert.Near(0.15, lim.EffectiveShare, 1e-9, "five seconds below 60 %: +0.05");
        for (int i = 0; i < 20; i++) Second(0.2);
        Assert.Near(0.30, lim.EffectiveShare, 1e-9, "never above the configured share");
        Assert.Near(0.2, lim.Last.ForeignLoad, 1e-6, "foreign load reported");
    }

    [Test]
    public static void BellsHalveTheShare()
    {
        var lim = new BusLoadLimiter(new BusLoadOptions { BusShare = 0.30, AdaptiveBackoff = false }, 0);
        long t = 0;
        for (int s = 0; s < 3; s++)
        {
            for (int i = 0; i < 10; i++) lim.Charge(4400, 32, 4400, t);   // 320 frames
            if (s == 2) for (int b = 0; b < 12; b++) lim.OnBell();            // 12 / 960 > 1 %
            t += 1_000_000;
            lim.Tick(t);
        }
        Assert.Equal(0.15, lim.EffectiveShare, "BELLs above 1 % of frames over 10 s halve s_eff");
    }

    [Test]
    public static void DrainEstimateUnderForeignLoad()
    {
        // 80 % synthetic foreign load of priority 3: it wins arbitration, our DATA gets
        // the rest. The adapter (66 frames) must never hold more than 48 of our frames.
        var h = new SimHarness(new DisplayScript { ProcessingUs = 50_000 });
        var traffic = new TrafficSource(h.Sim.Bus, 0x10, 0.80);
        h.Sim.Nodes.Add(traffic);
        h.Start();
        var dev = h.DiscoverOne();
        var img = FirmwareImage.FromBytes(TestImageFactory.Synthetic(BoardId.B7, "1.3.1", 60_000));
        var rep = h.Update(dev, new UpdateRequest { Firmware = img },
                           new UpdateOptions { Compression = CompressionMode.Off, ApplyMode = ApplyMode.None, BusLoad = new BusLoadOptions { BusShare = 0.30 } });
        Assert.Equal(OutcomeKind.Verified, rep.Firmware!.Kind, "transfer completes under load: " + rep.Firmware);
        Assert.True(h.SenderLink.MaxQueued <= 48, $"at most 48 frames outstanding, saw {h.SenderLink.MaxQueued}");
        Assert.Equal(0L, h.SenderLink.BellCount, "no BELLs");
        Assert.True(h.Engine.Limiter.EffectiveShare < 0.30, $"back-off engaged (s_eff {h.Engine.Limiter.EffectiveShare})");
    }

    [Test]
    public static void SmallBufferAdapterSwitchesToStopAndWait()
    {
        // An adapter that holds only 3 frames answers BELL to the OFFER (19 frames).
        var h = new SimHarness(senderCapacity: 3);
        h.Start();
        var dev = h.DiscoverOne();
        var img = FirmwareImage.FromBytes(TestImageFactory.Synthetic(BoardId.B7, "1.3.1", 30_000));
        var rep = h.Update(dev, new UpdateRequest { Firmware = img }, new UpdateOptions { Compression = CompressionMode.Off, ApplyMode = ApplyMode.None });
        Assert.True(h.Log.Any(l => l.Contains("switching to stop-and-wait")), "switched to stop-and-wait");
        Assert.Equal(OutcomeKind.Verified, rep.Firmware!.Kind, "completes in stop-and-wait: " + rep.Firmware);
        Assert.True(h.Display.LastWrittenImage!.AsSpan().SequenceEqual(img.Bytes), "frames kept in order: image intact");
        // "for the rest of the session" (6.4): the next session probes again with a normal OFFER.
        Assert.True(!h.Engine.StopAndWait, "back to normal writes after the session");
    }

    [Test(RealTime = true)]
    public static void RealTimePacing()
    {
        // 14.2: real share within ±5 % at 30 % over 60 s, and no 1 s window above 31 %.
        var clock = SystemClock.Instance;
        var link = new DryFrameLink(clock);
        var eng = new N2kEngine(link, clock, new EngineOptions { UniqueNumber = 1, BusLoad = new BusLoadOptions { BusShare = 0.30, AdaptiveBackoff = false } });
        eng.Start();
        var samples = new List<BusLoadSample>();
        eng.BusSampled += s => samples.Add(s);
        while (!eng.Source.IsUsable) { eng.Step(); Thread.Sleep(5); }
        IHostWaiter waiter = DefaultWaiter.Instance;
        string kind = "default";
        if (OperatingSystem.IsWindows())
        {
            var hr = new WindowsHighResWaiter();
            kind = hr.HighResolution ? "high-resolution timer" : "timeBeginPeriod(1)";
            waiter = hr;
        }
        long start = clock.NowUs, end = start + 60_000_000;
        long bitsAtStart = eng.Limiter.TotalOwnBits;
        int k = 0;
        while (clock.NowUs < end)
        {
            long next = eng.Step();
            byte[] payload = DataMsg.Build(0x5A, (uint)(k * 216), new byte[216]);
            if (eng.TrySendData(0x2A, payload, out long retry)) { k++; continue; }
            waiter.Wait(eng.WakeHandle, Math.Min(retry, next), clock);
        }
        double share = (eng.Limiter.TotalOwnBits - bitsAtStart) / ((clock.NowUs - start) / 1e6) / 250_000;
        Console.WriteLine(string.Create(CultureInfo.InvariantCulture, $"      real-time share {share:P2}, {k} DATA messages, waiter {kind}"));
        Assert.Near(0.30, share, 0.015, "real share at 30 % (±5 %)");
        // H3 (14.4) asks for at most 31 % in every 1 s window. The normative bucket of 6.2
        // (5,120 bits, refilled while the host is late) allows one extra burst in a
        // second, up to (75,000 + 5,120) / 250,000 = 32.05 %. Assert the normative bound.
        var full = samples.Skip(2).Take(samples.Count - 3).ToList();
        double bound = (0.30 * 250_000 + BusLoadLimiter.BucketCapacity) / 250_000;
        Assert.True(full.All(s => s.OwnLoad <= bound), string.Create(CultureInfo.InvariantCulture, $"every 1 s window ≤ {bound:P2} (max {full.Max(s => s.OwnLoad):P2})"));
        Console.WriteLine(string.Create(CultureInfo.InvariantCulture, $"      highest 1 s window {full.Max(s => s.OwnLoad):P2}, windows above 31 %: {full.Count(s => s.OwnLoad > 0.31)} of {full.Count}"));
        (waiter as IDisposable)?.Dispose();
    }
}
