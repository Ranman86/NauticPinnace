using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;
using NauticPinnace.N2kUpdate.Testing;

namespace NauticPinnace.N2kUpdate.Tests;

/// <summary>
/// The sender state machine (5.10) against the scripted display on a virtual bus:
/// every outcome of 5.7, the rows of 5.8, rejects, HOLD, lost frames, RESUME, ABORT.
/// </summary>
public static class SessionTests
{
    private static FirmwareImage Fw(string version = "1.3.1", int size = 60_000, int seed = 7, BoardId board = BoardId.B7) =>
        FirmwareImage.FromBytes(TestImageFactory.Synthetic(board, version, size, seed));

    private static (SimHarness H, DisplayDevice Dev) Rig(DisplayScript? script = null, int senderCapacity = SimHarness.CanableCapacity)
    {
        var h = new SimHarness(script, senderCapacity);
        h.Start();
        return (h, h.DiscoverOne());
    }

    private static UpdateOptions Opt(ApplyMode mode = ApplyMode.Now, CompressionMode comp = CompressionMode.Auto) =>
        new() { ApplyMode = mode, Compression = comp };

    private static string Why(SimHarness h, UpdateOutcome? o) =>
        $"{o}\n        display: {string.Join("\n                 ", h.Display.Events.TakeLast(6))}\n        sender:  {string.Join("\n                 ", h.Log.TakeLast(6))}";

    private static void Expect(SimHarness h, UpdateOutcome? o, OutcomeKind kind, Reason? reason = null)
    {
        Assert.True(o is not null && o.Kind == kind && (reason is null || o.Reason == reason),
                    $"expected {kind}{(reason is null ? "" : " " + reason)}, got {Why(h, o)}");
    }

    // ── Happy paths ───────────────────────────────────────────────────────────
    [Test]
    public static void FirmwareRawSuccess()
    {
        var (h, dev) = Rig();
        var fw = Fw();
        var rep = h.Update(dev, new UpdateRequest { Firmware = fw }, Opt(comp: CompressionMode.Off));
        Expect(h, rep.Firmware, OutcomeKind.Success);
        Assert.Equal(1, h.Display.RunningSlot, "running from the other slot");
        Assert.Equal(fw.ImageId, h.Display.RunningImageId, "running the new image");
        Assert.Equal(OtaState.Valid, h.Display.OtaState, "validated");
        Assert.Equal(1, h.Display.DialogsShown, "one dialog");
        Assert.True(h.Display.LastWrittenImage!.AsSpan().SequenceEqual(fw.Bytes), "image written intact");
        Assert.True(h.Progress.Any(p => p.MustStayConnected), "the UI was told to stay connected during validation");
        Assert.True(h.Progress.Any(p => p.Phase == SenderPhase.WaitingForConfirmation && p.DialogSecondsLeft is > 0), "dialog countdown reported");
    }

    [Test]
    public static void FirmwareDeflateWithCompanionWebUi()
    {
        var (h, dev) = Rig();
        var fw = Fw(size: 150_000);
        var web = WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi());
        var rep = h.Update(dev, new UpdateRequest { Firmware = fw, WebUi = web }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.Success);
        Expect(h, rep.WebUi, OutcomeKind.Success);
        Assert.Equal(1, h.Display.DialogsShown, "one confirmation for firmware and web UI (5.3.6)");
        Assert.Equal(web.WebUiId, h.Display.WebUiId, "new web UI installed");
        Assert.Equal("1.3.1", h.Display.WebUiVersion, "web UI version = firmware version");
        Assert.True(h.Log.Any(l => l.Contains("deflate")), "deflate was used");
    }

    [Test]
    public static void WebUiAloneWithDialog()
    {
        var (h, dev) = Rig();
        var web = WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi(seed: 5));
        var rep = h.Update(dev, new UpdateRequest { WebUi = web, WebUiVersion = "1.3.0" }, Opt());
        Expect(h, rep.WebUi, OutcomeKind.Success);
        Assert.Equal(1, h.Display.DialogsShown, "dialog for a lone web UI");
        Assert.Equal(web.WebUiId, h.Display.WebUiId, "installed");
        Assert.Equal(0, h.Display.Restarts, "no restart for a web UI");
    }

    [Test]
    public static void WebUiVersionMismatch()
    {
        var (h, dev) = Rig();
        var web = WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi(seed: 5));
        var rep = h.Update(dev, new UpdateRequest { WebUi = web, WebUiVersion = "1.4.0" }, Opt());
        Expect(h, rep.WebUi, OutcomeKind.Refused);
        Assert.Equal(0, h.Display.DialogsShown, "refused by the sender, nothing sent");
        var forced = h.Update(dev, new UpdateRequest { WebUi = web, WebUiVersion = "1.4.0" }, Opt() with { AllowWebUiVersionMismatch = true });
        Expect(h, forced.WebUi, OutcomeKind.Success);
        Assert.Equal("1.4.0", h.Display.WebUiVersion, "installed with ALLOW_VERSION_MISMATCH");
    }

    // ── Refusals and rejects ──────────────────────────────────────────────────
    [Test]
    public static void WrongBoardRefusedBySender()
    {
        var (h, dev) = Rig();
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(board: BoardId.Inch4) }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.Refused);
        Assert.Equal(0, h.Display.DialogsShown, "nothing offered");
    }

    [Test]
    public static void DeclinedAndTimedOut()
    {
        var (h, dev) = Rig(new DisplayScript { User = DisplayScript.UserAction.Decline });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.Rejected, Reason.DeclinedByUser);

        var (h2, dev2) = Rig(new DisplayScript { User = DisplayScript.UserAction.Ignore });
        long t0 = h2.Sim.NowUs;
        var rep2 = h2.Update(dev2, new UpdateRequest { Firmware = Fw() }, Opt());
        Expect(h2, rep2.Firmware, OutcomeKind.Rejected, Reason.ConfirmTimeout);
        Assert.True(h2.Sim.NowUs - t0 >= 60_000_000, "rejected after the 60 s dialog");
    }

    [Test]
    public static void EveryRejectReason()
    {
        Reason[] reasons =
        [
            Reason.Disabled, Reason.Busy, Reason.WrongBoard, Reason.TooLarge, Reason.UnsupportedTarget,
            Reason.UnsupportedEncoding, Reason.UnsupportedProtocol, Reason.BadRequest, Reason.PendingVerify,
            Reason.AlarmActive, Reason.WrongDevice, Reason.VersionMismatch, Reason.NoSpace, Reason.OutOfMemory,
            Reason.RestartPending, Reason.NotReady, Reason.InternalError,
        ];
        foreach (var r in reasons)
        {
            var (h, dev) = Rig(new DisplayScript { ForceReject = r });
            var rep = h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt());
            Expect(h, rep.Firmware, OutcomeKind.Rejected, r);
        }
        // And the real checks of the scripted display: settings off, alarm.
        var (h1, d1) = Rig();
        h1.Display.AcceptUpdates = false;
        Expect(h1, h1.Update(d1, new UpdateRequest { Firmware = Fw() }, Opt()).Firmware, OutcomeKind.Rejected, Reason.Disabled);
        var (h2, d2) = Rig();
        h2.Display.AlarmActive = true;
        Expect(h2, h2.Update(d2, new UpdateRequest { Firmware = Fw() }, Opt()).Firmware, OutcomeKind.Rejected, Reason.AlarmActive);
    }

    // ── Flow control ──────────────────────────────────────────────────────────
    [Test]
    public static void LongHoldIsWaitedOut()
    {
        // Flash work of 6 s per window announced as 400 ms: STATUS every second keeps the sender waiting.
        var (h, dev) = Rig(new DisplayScript { ProcessingUs = 6_000_000, HoldMs = 400 });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(size: 30_000) }, Opt(ApplyMode.None, CompressionMode.Off));
        Expect(h, rep.Firmware, OutcomeKind.Verified);
    }

    [Test]
    public static void LostFramesAreRecoveredByBitmap()
    {
        var (h, dev) = Rig();
        // Lose frame 3 of chunk 5 and frame 0 of chunk 17 once, on the wire.
        int lost = 0;
        var seen = new HashSet<(int, int)>();
        int dataMsg = -1;
        h.Sim.Bus.DropOnWire = (f, from) =>
        {
            if (CanId.Decode(f.Id).Priority != 7) return false;
            if ((f.Data[0] & 0x1F) == 0) dataMsg++;
            int idx = f.Data[0] & 0x1F;
            if ((dataMsg == 5 && idx == 3) || (dataMsg == 17 && idx == 0))
            {
                if (seen.Add((dataMsg, idx))) { lost++; return true; }
            }
            return false;
        };
        var fw = Fw(size: 40_000);
        var rep = h.Update(dev, new UpdateRequest { Firmware = fw }, Opt(ApplyMode.None, CompressionMode.Off));
        Expect(h, rep.Firmware, OutcomeKind.Verified);
        Assert.Equal(2, lost, "two frames lost");
        Assert.True(h.Display.LastWrittenImage!.AsSpan().SequenceEqual(fw.Bytes), "image intact");
    }

    [Test]
    public static void EverySeventhChunkLost()
    {
        var (h, dev) = Rig();
        int msg = -1;
        var dropped = new HashSet<int>();
        h.Sim.Bus.DropOnWire = (f, _) =>
        {
            if (CanId.Decode(f.Id).Priority != 7) return false;
            if ((f.Data[0] & 0x1F) == 0) msg++;
            return msg % 7 == 3 && (f.Data[0] & 0x1F) == 1 && dropped.Add(msg);
        };
        var fw = Fw(size: 80_000);
        var rep = h.Update(dev, new UpdateRequest { Firmware = fw }, Opt(ApplyMode.None, CompressionMode.Off));
        Expect(h, rep.Firmware, OutcomeKind.Verified);
        Assert.True(h.Display.LastWrittenImage!.AsSpan().SequenceEqual(fw.Bytes), "image intact");
    }

    [Test]
    public static void LostAckIsRecoveredByQuery()
    {
        var (h, dev) = Rig();
        int acks = 0;
        // Lose the display's second credit ACK (A2) completely.
        h.Sim.Bus.DropOnWire = (f, from) =>
        {
            if (from != h.DisplayLink || f.Data.Length < 5) return false;
            if ((f.Data[0] & 0x1F) == 0 && f.Data[4] == (byte)MsgType.Ack && f.Data[7] == 3) acks++;
            return acks == 2;
        };
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(size: 40_000) }, Opt(ApplyMode.None, CompressionMode.Off));
        Expect(h, rep.Firmware, OutcomeKind.Verified);
    }

    [Test]
    public static void LostOfferReplyIsRecovered()
    {
        var (h, dev) = Rig();
        bool dropped = false;
        h.Sim.Bus.DropOnWire = (f, from) =>
        {
            if (dropped || from != h.DisplayLink || f.Data.Length < 5) return false;
            if ((f.Data[0] & 0x1F) == 0 && f.Data[4] == (byte)MsgType.OfferReply) { dropped = true; return true; }
            return false;
        };
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(size: 20_000) }, Opt(ApplyMode.None));
        Expect(h, rep.Firmware, OutcomeKind.Verified);
        Assert.Equal(1, h.Display.DialogsShown, "the repeated OFFER was a duplicate, no second dialog");
    }

    [Test]
    public static void StaleAckIsIgnored()
    {
        var (h, dev) = Rig();
        var job = h.StartUpdate(dev, new UpdateRequest { Firmware = Fw(size: 60_000) }, Opt(ApplyMode.None, CompressionMode.Off));
        h.Sim.RunUntil(() => job.CurrentSession?.StateName == "Sending" && h.Display.State == SessionState.Receiving && h.Sim.NowUs > 12_000_000, h.Sim.NowUs + 60_000_000);
        // Inject an old ACK (base 0, bogus CRC) from the display's address.
        var inj = h.Sim.Bus.Attach("injector");
        inj.Open();
        byte sid = job.CurrentSession!.SessionId;
        var stale = new AckMsg { SessionId = sid, State = SessionState.Receiving, Base = 0, End = 13824, StreamCrc32 = 0x12345678 };
        h.Sim.RunFor(3_000_000);
        inj.SendBurst(FastPacket.Encode(CanId.Encode(6, UpdProto.Pgn, 0x2A, 230), stale.Encode(), 5));
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 600_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Verified);
    }

    // ── Cancel, abort, failures ───────────────────────────────────────────────
    [Test]
    public static void SenderCancels()
    {
        var (h, dev) = Rig();
        var job = h.StartUpdate(dev, new UpdateRequest { Firmware = Fw(size: 60_000) }, Opt(comp: CompressionMode.Off));
        h.Sim.RunUntil(() => h.Display.State == SessionState.Receiving, h.Sim.NowUs + 30_000_000);
        h.Sim.RunFor(1_000_000);
        job.Cancel(h.Sim.NowUs);
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 10_000_000);
        h.Sim.RunFor(1_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Aborted, Reason.SenderCancelled);
        Assert.Equal(SessionState.Failed, h.Display.State, "the display cleaned up");
    }

    [Test]
    public static void DisplayCancels()
    {
        var (h, dev) = Rig(new DisplayScript { CancelAtFraction = 0.4 });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(size: 60_000) }, Opt(comp: CompressionMode.Off));
        Expect(h, rep.Firmware, OutcomeKind.Aborted, Reason.UserCancelled);
        Assert.True(rep.Firmware!.ByDevice, "by the display");
    }

    [Test]
    public static void DisplayRestartsMidTransfer()
    {
        var (h, dev) = Rig(new DisplayScript { RestartAtFraction = 0.3 });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(size: 60_000) }, Opt(comp: CompressionMode.Off));
        Expect(h, rep.Firmware, OutcomeKind.Failed);
        Assert.True(rep.Firmware!.Message.Contains("restarted"), "reported as a display restart: " + rep.Firmware);
        Assert.Equal(SenderNote.DisplayRestarted, rep.Firmware.Note, "note for the UI");
        Assert.Equal(0, h.Display.RunningSlot, "old firmware still runs");
        // No RESUME into the restarting display: that would open a new dialog (5.3.4).
        Assert.Equal(1, h.Display.DialogsShown, "no dialog after the restart");
    }

    [Test]
    public static void ShaMismatchReportedByDisplay()
    {
        var (h, dev) = Rig(new DisplayScript { CorruptFlash = true });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.Failed, Reason.ShaMismatch);
        Assert.Equal(0, h.Display.Restarts, "no restart");
    }

    // ── Outcomes after the restart (5.7) ──────────────────────────────────────
    [Test]
    public static void SameVersionReinstall()
    {
        var (h, dev) = Rig();
        var fw = Fw(version: "1.3.0");
        h.Display.SetRunning("1.3.0", fw.ImageId);   // the very same build runs already
        dev = h.DiscoverOne();
        var rep = h.Update(dev, new UpdateRequest { Firmware = fw }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.Success);
        Assert.Equal(1, h.Display.RunningSlot, "decided by slot, not by version or image id");
    }

    [Test]
    public static void RolledBackAfterFailedHealthCheck()
    {
        var (h, dev) = Rig(new DisplayScript { FailHealthCriterion = 3 });
        var fw = Fw();
        var rep = h.Update(dev, new UpdateRequest { Firmware = fw, WebUi = WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi()) }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.RolledBack);
        Assert.Equal(RollbackCause.HealthCheck, rep.Firmware!.Cause, "cause 1");
        Assert.Equal((byte)3, rep.Firmware.Detail, "detail = the criterion");
        Expect(h, rep.WebUi, OutcomeKind.Skipped);
        Assert.Equal(0, h.Display.RunningSlot, "old firmware runs again");
    }

    [Test]
    public static void RolledBackWithoutReceptionProof()
    {
        // H5: with mode NowNoWait the display needs any frame; with Now it needs our polls.
        // Script the display to fail H5 regardless: the sender reports cause 1 detail 5.
        var (h, dev) = Rig(new DisplayScript { FailHealthCriterion = 5 });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.RolledBack);
        Assert.Equal((byte)5, rep.Firmware!.Detail, "detail 5 = no reception");
    }

    [Test]
    public static void RestartPostponedOnTheDisplay()
    {
        var (h, dev) = Rig(new DisplayScript { Restart = DisplayScript.RestartChoice.Later });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.InstalledRestartPostponed);
        Assert.Equal(0, h.Display.Restarts, "no restart");
    }

    [Test]
    public static void CountdownIsNotMistakenForPostponed()
    {
        // During the 10 s countdown INFO already shows "restart pending" (4.5); the
        // sender must keep polling instead of reporting "postponed".
        var (h, dev) = Rig(new DisplayScript { Restart = DisplayScript.RestartChoice.Countdown });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.Success);
    }

    [Test]
    public static void ApplyModesWithoutPolling()
    {
        var (h, dev) = Rig();
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt(ApplyMode.NextRestart));
        Expect(h, rep.Firmware, OutcomeKind.InstalledNotConfirmed);
        Assert.Equal(0, h.Display.Restarts, "mode 1: no restart");

        // Mode 2: H5 accepts any N2K frame, here from the instruments on the bus. Without
        // any other traffic the display would roll back (10.2, consequences of H5).
        var (h2, dev2) = Rig();
        h2.Sim.Nodes.Add(new TrafficSource(h2.Sim.Bus, 0x11, 0.05));
        var rep2 = h2.Update(dev2, new UpdateRequest { Firmware = Fw() }, Opt(ApplyMode.NowNoWait));
        Expect(h2, rep2.Firmware, OutcomeKind.InstalledNotConfirmed);
        h2.Sim.RunFor(100_000_000);
        Assert.Equal(OtaState.Valid, h2.Display.OtaState, "mode 2: validated by any N2K frame");

        var (h4, dev4) = Rig();
        var rep4 = h4.Update(dev4, new UpdateRequest { Firmware = Fw() }, Opt(ApplyMode.NowNoWait));
        Expect(h4, rep4.Firmware, OutcomeKind.InstalledNotConfirmed);
        h4.Sim.RunFor(260_000_000);
        Assert.Equal(0, h4.Display.RunningSlot, "mode 2 on a silent bench bus: rolled back");

        var (h3, dev3) = Rig();
        var rep3 = h3.Update(dev3, new UpdateRequest { Firmware = Fw() }, Opt(ApplyMode.None));
        Expect(h3, rep3.Firmware, OutcomeKind.Verified);
    }

    [Test]
    public static void DisplayNeverComesBack()
    {
        var (h, dev) = Rig();
        var job = h.StartUpdate(dev, new UpdateRequest { Firmware = Fw() }, Opt());
        h.Sim.RunUntil(() => h.Display.Restarts > 0, h.Sim.NowUs + 600_000_000);
        h.Display.PowerOff();   // switched off for good
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 600_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Unknown);
    }

    [Test]
    public static void NewAddressAfterRestart()
    {
        var (h, dev) = Rig(new DisplayScript { AddressAfterBoot = 0x30 });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(), WebUi = WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi()) }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.Success);
        Expect(h, rep.WebUi, OutcomeKind.Success);
        Assert.Equal(0x30, rep.Device.Address, "followed the display by unique id");
    }

    // ── Recovery (5.8) ────────────────────────────────────────────────────────
    [Test]
    public static void SenderRestartResumesWithoutNewDialog()
    {
        var (h, dev) = Rig();
        var fw = Fw(size: 120_000);
        string cache = Path.Combine(Path.GetTempPath(), "n2kupd-resume-" + Guid.NewGuid().ToString("N"));
        try
        {
            var opt = Opt() with { CacheDirectory = cache };
            var job = h.StartUpdate(dev, new UpdateRequest { Firmware = fw }, opt);
            h.Sim.RunUntil(() => (job.CurrentSession?.LatestInfo is not null) && h.Display.State == SessionState.Receiving && h.Sim.NowUs > 15_000_000, h.Sim.NowUs + 60_000_000);
            h.Sim.RunFor(2_000_000);
            Assert.True(!job.IsFinished, "still transferring");

            // Kill the sender: its node disappears, its adapter goes quiet.
            h.Sim.Nodes.RemoveAll(n => n is EngineNode);
            h.SenderLink.Close();
            h.Sim.RunFor(8_000_000);

            // A new run with a new engine and adapter, the same file and cache.
            var link2 = h.Sim.Bus.Attach("sender-2", SimHarness.CanableCapacity);
            var eng2 = new N2kEngine(link2, h.Sim.Clock, new EngineOptions { UniqueNumber = 0x0ACE5 }, new Random(99));
            eng2.Log += m => h.Log.Add("[run 2] " + m);
            h.Sim.Nodes.Add(new EngineNode(eng2));
            eng2.Start();
            var disc = new DiscoveryOperation(eng2);
            eng2.Add(disc);
            h.Sim.RunUntil(() => disc.IsFinished, h.Sim.NowUs + 10_000_000);
            var dev2 = disc.Result!.Displays.Single();
            Assert.True(dev2.Info.SessionState is SessionState.Receiving or SessionState.Processing, "the display still holds the session");
            var job2 = new UpdateJob(eng2, dev2, new UpdateRequest { Firmware = fw }, opt);
            eng2.Add(job2);
            h.Sim.RunUntil(() => job2.IsFinished, h.Sim.NowUs + 1_200_000_000);
            Expect(h, job2.Report!.Firmware, OutcomeKind.Success);
            Assert.Equal(1, h.Display.DialogsShown, "RESUME without a new confirmation");
        }
        finally { if (Directory.Exists(cache)) Directory.Delete(cache, true); }
    }

    [Test]
    public static void AdapterUnpluggedMidTransfer()
    {
        var (h, dev) = Rig();
        var job = h.StartUpdate(dev, new UpdateRequest { Firmware = Fw(size: 120_000) }, Opt(ApplyMode.None, CompressionMode.Off));
        h.Sim.RunUntil(() => h.Display.State == SessionState.Receiving && h.Sim.NowUs > 14_000_000, h.Sim.NowUs + 60_000_000);
        h.SenderLink.Unplug();
        h.Sim.RunFor(10_000_000);
        h.SenderLink.Unplugged = false;   // plugged in again; the library reopens it
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 600_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Verified);
        Assert.Equal(1, h.Display.DialogsShown, "resumed without a new dialog");
    }

    [Test]
    public static void UsbWriteErrorMidTransfer()
    {
        // The adapter stays open but one write fails: the library reopens it (its own
        // Close() reports Closed, which must not start another round) and resumes.
        var (h, dev) = Rig();
        var job = h.StartUpdate(dev, new UpdateRequest { Firmware = Fw(size: 120_000) }, Opt(ApplyMode.None, CompressionMode.Off));
        h.Sim.RunUntil(() => h.Display.State == SessionState.Receiving && h.Sim.NowUs > 14_000_000, h.Sim.NowUs + 60_000_000);
        h.SenderLink.FailNextWrite = true;
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 600_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Verified);
        Assert.Equal(1, h.Log.Count(l => l.Contains("CAN link reopened")), "reopened exactly once");
        Assert.Equal(1, h.Display.DialogsShown, "resumed without a new dialog");
    }

    [Test]
    public static void SenderLosesItsAddressMidTransfer()
    {
        var (h, dev) = Rig();
        var job = h.StartUpdate(dev, new UpdateRequest { Firmware = Fw(size: 120_000) }, Opt(ApplyMode.None, CompressionMode.Off));
        h.Sim.RunUntil(() => h.Display.State == SessionState.Receiving && h.Sim.NowUs > 14_000_000, h.Sim.NowUs + 60_000_000);
        var rogue = h.Sim.Bus.Attach("rogue");
        rogue.Open();
        rogue.SendBurst([new CanFrame(CanId.Encode(6, N2kPgn.IsoAddressClaim, 230, 255), N2kName.Create(1, 135, 130, 25).ToBytes())]);
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 600_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Verified);
        Assert.Equal(231, h.Engine.Source.Address, "moved to 231");
        Assert.Equal(1, h.Display.DialogsShown, "RESUME from the new address, no new dialog");
    }

    [Test]
    public static void PausesForAnotherTransfer()
    {
        var (h, dev) = Rig();
        var job = h.StartUpdate(dev, new UpdateRequest { Firmware = Fw(size: 120_000) }, Opt(ApplyMode.None, CompressionMode.Off));
        h.Sim.RunUntil(() => h.Display.State == SessionState.Receiving && h.Sim.NowUs > 14_000_000, h.Sim.NowUs + 60_000_000);
        // Another sender (0x50) sends DATA to another display (0x31) for 3 s.
        var other = h.Sim.Bus.Attach("other-sender");
        other.Open();
        long foreignStart = h.Sim.NowUs, foreignEnd = foreignStart + 3_000_000;
        var dataFrame0 = new CanFrame(CanId.Encode(7, UpdProto.Pgn, 0x50, 0x31), [0, 0xDF, 0xFE, 0x9F, 0xA4, 0x11, 0, 0]);
        long framesAtStart = 0;
        while (h.Sim.NowUs < foreignEnd)
        {
            other.SendBurst([dataFrame0]);
            h.Sim.RunFor(200_000);
            if (framesAtStart == 0) framesAtStart = h.SenderLink.FramesWritten;
        }
        long during = h.SenderLink.FramesWritten - framesAtStart;
        h.Sim.RunFor(9_000_000);   // still within the 10 s quiet time
        long quiet = h.SenderLink.FramesWritten - framesAtStart - during;
        Assert.True(during < 40 && quiet < 60, $"no DATA while another transfer runs ({during} / {quiet} frames, keepalives only)");
        Assert.True(h.Log.Any(l => l.Contains("pausing")), "paused");
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 600_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Verified);
    }

    // ── Several displays ──────────────────────────────────────────────────────
    [Test]
    public static void TwoDisplaysOneAfterAnother()
    {
        var h = new SimHarness();
        var link2 = h.Sim.Bus.Attach("display-2");
        var d2 = new ScriptedDisplay(link2, h.Sim.Clock) { Address = 0x31, Uid = new UniqueId(0x020000000002), Label = "Navi" };
        h.Sim.Nodes.Add(d2);
        h.Start();
        var found = h.Discover();
        Assert.Equal(2, found.Displays.Count, "both displays found");
        var fw = Fw();
        foreach (var dev in found.Displays)
        {
            var rep = h.Update(dev, new UpdateRequest { Firmware = fw }, Opt());
            Expect(h, rep.Firmware, OutcomeKind.Success);
        }
        Assert.Equal(fw.ImageId, h.Display.RunningImageId, "display 1 updated");
        Assert.Equal(fw.ImageId, d2.RunningImageId, "display 2 updated");
        Assert.Equal(1, h.Display.DialogsShown, "one dialog on display 1");
        Assert.Equal(1, d2.DialogsShown, "one dialog on display 2");
    }

    [Test]
    public static void IdentifyAndLegacyDiscovery()
    {
        var h = new SimHarness();
        var link2 = h.Sim.Bus.Attach("display-2");
        var d2 = new ScriptedDisplay(link2, h.Sim.Clock) { Address = 0x31, Uid = new UniqueId(0x020000000002) };
        h.Sim.Nodes.Add(d2);
        // A 1.2.0 display: answers PGN 126996, not QUERY(INFO).
        var legacyLink = h.Sim.Bus.Attach("legacy");
        legacyLink.Open();
        legacyLink.FrameReceived += (f, _) =>
        {
            var (pgn, _, src, dst) = CanId.Decode(f.Id);
            if (pgn == N2kPgn.IsoRequest && f.Data.Length >= 3 && (f.Data[0] | f.Data[1] << 8 | f.Data[2] << 16) == N2kPgn.ProductInfo)
            {
                var pi = new ProductInfo { ProductCode = 100, ModelId = "NauticPinnace", SoftwareVersion = "1.2.0" }.Encode();
                legacyLink.SendBurst(FastPacket.Encode(CanId.Encode(6, N2kPgn.ProductInfo, 0x40, 255), pi, 0));
            }
        };
        h.Start();
        var found = h.Discover();
        Assert.Equal(2, found.Displays.Count, "two displays with a receiver");
        Assert.Equal(1, found.Legacy.Count, "one legacy display");
        Assert.Equal("1.2.0", found.Legacy[0].SoftwareVersion, "its version");

        var target = found.Displays.First(d => d.Uid == d2.Uid);
        var op = new IdentifyOperation(h.Engine, target, 10);
        h.Engine.Add(op);
        h.Sim.RunFor(500_000);
        Assert.Equal(1, d2.IdentifyShown, "the targeted display identifies itself");
        Assert.Equal(0, h.Display.IdentifyShown, "the other one does not");
    }

    // ── Two senders, cancel, adapter between updates ──────────────────────────
    [Test]
    public static void SecondSenderDoesNotTakeOverARunningSession()
    {
        // Sender A transfers; sender B starts the same release. B sees the held session
        // in INFO, but it is not one B opened: no RESUME, a plain OFFER, BUSY (5.8). (A
        // RESUME from B would be BUSY too while A is heard - the display's side, 5.3.4.)
        var h = new SimHarness();
        var linkB = h.Sim.Bus.Attach("sender-B", SimHarness.CanableCapacity);
        var engB = new N2kEngine(linkB, h.Sim.Clock, new EngineOptions { UniqueNumber = 0x1234, PreferredAddress = 231 }, new Random(2));
        h.Sim.Nodes.Add(new EngineNode(engB));
        h.Start();
        engB.Start();
        h.Sim.RunFor(300_000);
        var devA = h.DiscoverOne();
        var fw = Fw(size: 300_000);
        var jobA = h.StartUpdate(devA, new UpdateRequest { Firmware = fw }, Opt(ApplyMode.None));
        h.Sim.RunUntil(() => h.Display.State == SessionState.Receiving, h.Sim.NowUs + 30_000_000);
        h.Sim.RunFor(5_000_000);
        var disc = new DiscoveryOperation(engB);
        engB.Add(disc);
        h.Sim.RunUntil(() => disc.IsFinished, h.Sim.NowUs + 10_000_000);
        var devB = disc.Result!.Displays.Single();
        Assert.True(devB.Info.SessionState is >= SessionState.Confirming and <= SessionState.Done, "B sees the held session");
        var jobB = new UpdateJob(engB, devB, new UpdateRequest { Firmware = fw }, Opt(ApplyMode.None));
        engB.Add(jobB);
        h.Sim.RunUntil(() => jobA.IsFinished && jobB.IsFinished, h.Sim.NowUs + 600_000_000);
        Expect(h, jobB.Report!.Firmware, OutcomeKind.Rejected, Reason.Busy);
        Expect(h, jobA.Report!.Firmware, OutcomeKind.Verified);
        Assert.True(!h.Display.Events.Any(e => e.Contains("RESUME")), "no rebind to B");
    }

    [Test]
    public static void CancelBeforeTheFirstTickSendsNothing()
    {
        // UpdateAsync posts Add(job) and, if the token was cancelled while the images were
        // prepared, the cancel right after it: no OFFER may go out (5.10, last paragraph).
        var (h, dev) = Rig();
        var job = new UpdateJob(h.Engine, dev, new UpdateRequest { Firmware = Fw() }, Opt());
        h.Engine.Post(() => h.Engine.Add(job));
        h.Engine.Post(() => job.Cancel(h.Sim.Clock.NowUs));
        h.Sim.RunUntil(() => job.IsFinished, h.Sim.NowUs + 5_000_000);
        Expect(h, job.Report!.Firmware, OutcomeKind.Aborted, Reason.SenderCancelled);
        Assert.Equal(0, h.Display.DialogsShown, "no OFFER, no dialog");
    }

    [Test]
    public static void StayConnectedFromTheApply()
    {
        // From APPLY(mode 0) on the display may count down already: the UI must warn
        // before closing then too, not only in WAIT_REBOOT (H5, 12.7).
        var (h, dev) = Rig();
        h.Update(dev, new UpdateRequest { Firmware = Fw() }, Opt());
        Assert.True(h.Progress.Any(p => p.Phase == SenderPhase.Applying && p.MustStayConnected), "warned while applying");
        Assert.True(!h.Progress.Any(p => p.Phase == SenderPhase.Transferring && p.MustStayConnected), "not during the transfer");
    }

    [Test]
    public static void WebUiAfterPostponedRestartSaysWhy()
    {
        var (h, dev) = Rig(new DisplayScript { Restart = DisplayScript.RestartChoice.Later });
        var rep = h.Update(dev, new UpdateRequest { Firmware = Fw(), WebUi = WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi()) }, Opt());
        Expect(h, rep.Firmware, OutcomeKind.InstalledRestartPostponed);
        Expect(h, rep.WebUi, OutcomeKind.Skipped);
        Assert.Equal(SenderNote.FirmwareActiveAfterRestart, rep.WebUi!.Note, "skipped because the firmware is not active yet");
    }

    [Test]
    public static void DiscoveryReopensAnAdapterPluggedBackWhileIdle()
    {
        var (h, _) = Rig();
        h.SenderLink.Unplug();
        h.Sim.RunFor(1_000_000);
        h.SenderLink.Unplugged = false;   // plugged in again; no update runs to reopen it
        var r = h.Discover();
        Assert.Equal(1, r.Displays.Count, "found after the replug");
        Assert.True(!r.AdapterLost, "adapter reopened");
        h.SenderLink.Unplug();
        var r2 = h.Discover();
        Assert.True(r2.AdapterLost, "reported when it stays away");
    }

    [Test]
    public static void SharedSourceAddressIsUsedAsIs()
    {
        // 12.3: NauticPi's own claim on the same CANable; the engine claims nothing itself.
        var sim = new Simulation();
        var link = sim.Bus.Attach("nauticpi", SimHarness.CanableCapacity);
        var displayLink = sim.Bus.Attach("display");
        var display = new ScriptedDisplay(displayLink, sim.Clock);
        var src = new FixedSource(220);
        var eng = new N2kEngine(link, sim.Clock, new EngineOptions { UniqueNumber = 7 }, new Random(3), src);
        var claims = 0;
        displayLink.FrameReceived += (f, _) => { if (CanId.Decode(f.Id).Pgn == N2kPgn.IsoAddressClaim) claims++; };
        sim.Nodes.Add(new EngineNode(eng));
        sim.Nodes.Add(display);
        eng.Start();
        var disc = new DiscoveryOperation(eng);
        eng.Add(disc);
        sim.RunUntil(() => disc.IsFinished, sim.NowUs + 10_000_000);
        Assert.Equal(1, disc.Result!.Displays.Count, "discovery works with the shared address");
        Assert.True(eng.Claimer is null && claims == 0, "no address claim of its own");
        eng.NoteExternalTransmit(new CanFrame(CanId.Encode(2, 127250, 220, 255), new byte[8]));
        sim.RunFor(1_100_000);
        Assert.True(eng.Limiter.Last.TotalLoad > 0, "the owner's own frames count towards the total load");
    }

    private sealed class FixedSource(int address) : ISourceAddress
    {
        public int Address => address;
        public bool IsUsable => true;
        public event Action<int, int>? Changed { add { } remove { } }
    }
}
