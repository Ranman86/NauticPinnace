using System.Collections.Concurrent;
using System.Security.Cryptography;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Testing;

/// <summary>What the scripted user and the scripted hardware do.</summary>
public sealed class DisplayScript
{
    public enum UserAction { Accept, Decline, Ignore }
    public enum RestartChoice { Countdown, RestartNow, Later }

    public UserAction User { get; set; } = UserAction.Accept;
    /// <summary>When the user's decision lands after the dialog opens (≥ 3 s: 1.5 s guard + 1.5 s hold).</summary>
    public long UserDelayUs { get; set; } = 4_000_000;
    public RestartChoice Restart { get; set; } = RestartChoice.Countdown;

    /// <summary>Reject every OFFER with this reason after the regular checks pass.</summary>
    public Reason? ForceReject { get; set; }

    public long PrepareUs { get; set; } = 120_000;
    /// <summary>Flash work per window; the A1 ACK announces <see cref="HoldMs"/>.</summary>
    public long ProcessingUs { get; set; } = 300_000;
    public ushort HoldMs { get; set; } = 400;
    public long VerifyUs { get; set; } = 1_500_000;
    public long ApplyUs { get; set; } = 200_000;
    /// <summary>Silent time after a restart (boot until setupDone).</summary>
    public long BootUs { get; set; } = 15_000_000;

    /// <summary>Flip one byte of the written image: RESULT SHA_MISMATCH at read-back (5.5 step 5).</summary>
    public bool CorruptFlash { get; set; }
    /// <summary>Cancel on the display once this fraction of the stream is accepted.</summary>
    public double? CancelAtFraction { get; set; }
    /// <summary>Power-cycle once this fraction of the stream is accepted (5.8 display restarts).</summary>
    public double? RestartAtFraction { get; set; }
    /// <summary>Health check fails after the restart: rollback with cause 1 and this detail.</summary>
    public byte? FailHealthCriterion { get; set; }
    /// <summary>The address the display claims after its next restart.</summary>
    public int? AddressAfterBoot { get; set; }
    /// <summary>Do not answer the n-th QUERY of any kind (1-based); for lost-reply tests.</summary>
    public HashSet<int> IgnoreQueries { get; } = new();
}

/// <summary>
/// A display that answers per docs/N2K-UPDATE.md: acceptance rules (5.1), OFFER
/// checks in the order of 5.3.1, dialog and RESUME (5.3), windows and ACKs A1-A6
/// (5.4), verification (5.5), APPLY (5.6), the restart with OtaGuard and H5 (10.2),
/// rollback reporting (10.5) and the companion approval (5.3.6). Deterministic, so
/// its frames can serve as the expected replies of the cross test (14.3.2). It is a
/// reference for the sender, not a port of the firmware: flash and decoding are
/// modelled, not emulated.
/// </summary>
public sealed class ScriptedDisplay : ISimNode
{
    private sealed class Slot
    {
        public ImageId Id;
        public string Version = "";
        public byte[]? Bytes;
    }

    private sealed class Sess
    {
        public int Sender;
        public byte Sid;
        public OfferMsg Offer = null!;
        public byte[] OfferRaw = [];
        public SessionState State;
        public Reason LastReason;
        public long StateSince;
        public long LastHeard;
        public long DialogEnd;
        public long UserAt;
        public long Due;              // next scheduled step of the current state
        public long NextStatus;
        public byte[] Stream = [];
        public uint Base, End;
        public ulong Bitmap;
        public uint Crc;              // CRC over [0, Base)
        public long LastData;
        public int A4Count;
        public long LastA6;
        public int BelowBase;
        public int Malformed;
        public ResultMsg? Result;
        public long LastResult;
        public byte[]? Decoded;
        public bool ViaCompanion;
        public ApplyWireMode ApplyMode;
    }

    private readonly ICanFrameLink _link;
    private readonly IClock _clock;
    private readonly ConcurrentQueue<(CanFrame F, long Ts)> _rx = new();
    private readonly FastPacketAssembler _asm = new() { GapToleranceUs = 2_000_000 };
    private readonly FastPacketSequencer _seq = new();
    private readonly Slot[] _slots = [new Slot(), new Slot()];
    private Sess? _s;
    private long _lastSessionUnknown = long.MinValue / 2;
    private long _lastIdentify = long.MinValue / 2;
    private int _queryCount;

    // Persistent state ("flash" and NVS).
    private int _bootSlot;
    private bool _restartPending;
    private (ImageId Id, byte Mode)? _h5;
    private (byte[] Sha, uint Size, ImageId FwId, string Version)? _companion;
    private long _companionUntil = long.MinValue;
    private (string Version, ImageId Id, RollbackCause Cause, byte Detail)? _rbInfo;

    // Per boot.
    private long _bootUs;
    private bool _booting;
    private bool _protoSinceBoot, _anySinceBoot;
    private long _restartAt = long.MaxValue;
    private bool _claimSent;

    public ScriptedDisplay(ICanFrameLink link, IClock clock, DisplayScript? script = null)
    {
        _link = link;
        _clock = clock;
        Script = script ?? new DisplayScript();
        link.FrameReceived += (f, ts) => { _rx.Enqueue((f, ts)); Received?.Invoke(); };
        if (!link.IsOpen) link.Open();
        _bootUs = clock.NowUs - 754_000_000L;   // has been running for a while
        _slots[0] = new Slot { Id = new ImageId(0x1122334455667788), Version = "1.3.0" };
    }

    public DisplayScript Script { get; }

    /// <summary>Raised on the link's thread for every received frame (to wake a real-time driver).</summary>
    public event Action? Received;

    // ── Identity and configuration ────────────────────────────────────────────
    public int Address { get; set; } = 0x2A;
    public UniqueId Uid { get; set; } = new(0x020000000001);
    public BoardId Board { get; set; } = BoardId.B7;
    public string Label { get; set; } = "S/V Example";
    public Capabilities Caps { get; set; } = (Capabilities)0x3F;
    public uint MaxFirmwareSize { get; set; } = 0x4EF000;
    public uint MaxWebUiSize { get; set; } = 0x20000;
    public bool AcceptUpdates { get; set; } = true;
    public bool Ready { get; set; } = true;
    public bool AlarmActive { get; set; }
    public N2kName Name => N2kName.Create(0x2_0001, 2046, 120, 120);

    public int RunningSlot { get; private set; }
    public OtaState OtaState { get; private set; } = OtaState.Valid;
    public string RunningVersion => _slots[RunningSlot].Version;
    public ImageId RunningImageId => _slots[RunningSlot].Id;

    public byte[]? WebUi { get; private set; }
    public string WebUiVersion { get; private set; } = "1.3.0";
    public ImageId WebUiId { get; private set; } = new(0x37558A1A25D09B4D);
    public uint WebUiSize { get; private set; } = 48_751;

    // ── Observations for tests ────────────────────────────────────────────────
    public int DialogsShown { get; private set; }
    public int IdentifyShown { get; private set; }
    public int Restarts { get; private set; }
    public List<string> Events { get; } = new();
    public SessionState State => _s?.State ?? SessionState.Idle;
    public byte[]? LastWrittenImage { get; private set; }
    public bool IsBooting => _booting;

    /// <summary>Switched off: no step, no frame, nothing received.</summary>
    public bool PoweredOff { get; private set; }

    public void PowerOff()
    {
        PoweredOff = true;
        _rx.Clear();
    }

    /// <summary>Sets the image the display is running (slot 0), e.g. to match a release's previous version.</summary>
    public void SetRunning(string version, ImageId id, int slot = 0)
    {
        RunningSlot = slot;
        _bootSlot = slot;
        _slots[slot] = new Slot { Id = id, Version = version };
    }

    public void SetWebUi(byte[] gz, string version)
    {
        WebUi = gz;
        WebUiVersion = version;
        WebUiId = ImageId.Read(SHA256.HashData(gz));
        WebUiSize = (uint)gz.Length;
    }

    private uint Uptime(long now) => (uint)Math.Max(0, (now - _bootUs) / 1_000_000);

    private void Log(long now, string what) => Events.Add($"{now / 1000,9} ms  {what}");

    // ── Step ──────────────────────────────────────────────────────────────────
    public long Step(long now)
    {
        if (PoweredOff) { _rx.Clear(); return long.MaxValue; }
        if (_booting)
        {
            _rx.Clear();
            if (now < _bootUs + Script.BootUs) return _bootUs + Script.BootUs;
            _booting = false;
            Log(now, $"setup done, address {Address}, slot {RunningSlot}, {OtaState}");
        }
        if (!_claimSent)
        {
            _claimSent = true;
            SendRaw(CanId.Encode(6, N2kPgn.IsoAddressClaim, Address, 255), Name.ToBytes());
        }
        while (_rx.TryDequeue(out var rx)) OnFrame(rx.F, rx.Ts);
        if (_booting) return _bootUs + Script.BootUs;

        long next = long.MaxValue;
        next = Math.Min(next, TickGuard(now));
        if (_booting) return _bootUs + Script.BootUs;
        next = Math.Min(next, TickSession(now));
        if (now >= _restartAt) { Reboot(now, "countdown"); return _bootUs + Script.BootUs; }
        next = Math.Min(next, _restartAt);
        return next;
    }

    private void OnFrame(CanFrame f, long ts)
    {
        var (pgn, _, src, dst) = CanId.Decode(f.Id);
        _anySinceBoot = true;
        if (pgn == N2kPgn.IsoRequest && f.Length >= 3 && (dst == Address || dst == 255))
        {
            int req = f.Data[0] | (f.Data[1] << 8) | (f.Data[2] << 16);
            if (req == N2kPgn.IsoAddressClaim) SendRaw(CanId.Encode(6, N2kPgn.IsoAddressClaim, Address, 255), Name.ToBytes());
            else if (req == N2kPgn.ProductInfo)
                SendPgn(N2kPgn.ProductInfo, src, new ProductInfo { ProductCode = 100, ModelId = "NauticPinnace", SoftwareVersion = RunningVersion, ModelVersion = "1.1", SerialCode = "NAUTICPINNACE-001" }.Encode());
            return;
        }
        if (pgn != UpdProto.Pgn) return;
        // The receiving library reassembles every 126720 message, whatever the destination (2.4).
        byte[]? p = _asm.Add(src, dst, pgn, f.Data, ts);
        if (p is null || p.Length < 3 || p[0] != UpdProto.Mfg0 || p[1] != UpdProto.Mfg1) return;
        OnPayload(src, dst, p, ts);
    }

    private void OnPayload(int src, int dst, byte[] raw, long now)
    {
        var msg = UpdateMessage.Decode(raw);
        if (msg is null || !UpdateMessage.IsSenderToDisplay(msg.Type)) return;
        // 5.1 rule 2: addressed to us; only QUERY may be broadcast.
        if (dst != Address && !(dst == 255 && msg.Type == MsgType.Query)) return;
        if (msg is QueryMsg q0 && !q0.UidFilter.IsAny && q0.UidFilter != Uid) return;
        bool versionOk = msg.Type == MsgType.Data || msg.ProtocolVersion == UpdProto.Version;
        if (versionOk) _protoSinceBoot = true;   // H5 PROTOCOL reception proof (10.2)
        if (!versionOk && !(msg is QueryMsg { What: QueryWhat.Info })) return;   // 5.1 rule 3

        switch (msg)
        {
            case QueryMsg q: OnQuery(q, src, now); break;
            case IdentifyMsg id: OnIdentify(id, now); break;
            case OfferMsg o: OnOffer(o, raw, src, now); break;
            case DataMsg d: OnData(d, raw.Length - UpdProto.DataHeaderLength, src, now); break;
            case ApplyMsg a: OnApply(a, src, now); break;
            case AbortMsg ab: OnAbort(ab, src, now); break;
        }
    }

    private bool Bound(int src, byte sid) => _s is not null && _s.Sender == src && _s.Sid == sid;

    private void SessionUnknown(int src, byte sid, long now)
    {
        if (now - _lastSessionUnknown < 1_000_000) return;
        _lastSessionUnknown = now;
        Send(src, new AbortMsg { SessionId = sid, Reason = Reason.SessionUnknown });
    }

    // ── QUERY, IDENTIFY ───────────────────────────────────────────────────────
    private void OnQuery(QueryMsg q, int src, long now)
    {
        _queryCount++;
        if (Script.IgnoreQueries.Contains(_queryCount)) { Log(now, $"query #{_queryCount} ignored (script)"); return; }
        if (q.What == QueryWhat.Info) { Send(src, BuildInfo(now)); return; }
        if (q.SessionId != 0 && !Bound(src, q.SessionId))
        {
            SessionUnknown(src, q.SessionId, now);
            return;
        }
        if (_s is not null && q.SessionId != 0) _s.LastHeard = now;
        switch (q.What)
        {
            case QueryWhat.Status:
                Send(src, BuildStatus(now));
                break;
            case QueryWhat.Ack:
                if (_s is { State: SessionState.Receiving or SessionState.Processing }) Send(src, BuildAck(_s));
                else Send(src, BuildStatus(now));
                break;
            case QueryWhat.Result:
                if (_s?.Result is { } r) Send(src, r);
                else Send(src, BuildStatus(now));
                break;
        }
    }

    private void OnIdentify(IdentifyMsg m, long now)
    {
        if (m.TargetUid != Uid) return;
        if (_s is { State: SessionState.Confirming } || _restartAt != long.MaxValue || AlarmActive) return;
        if (now - _lastIdentify < 10_000_000) return;
        _lastIdentify = now;
        IdentifyShown++;
        Log(now, $"identify screen for {Math.Min(30, (int)m.Seconds)} s");
    }

    // ── OFFER (5.3) ───────────────────────────────────────────────────────────
    private void Reply(int src, byte sid, OfferDecision d, Reason r = Reason.Ok, ushort secs = 0, byte window = 0) =>
        Send(src, new OfferReplyMsg { SessionId = sid, Decision = d, Reason = r, Seconds = secs, WindowChunks = window });

    private void Reject(int src, byte sid, Reason r, long now)
    {
        Log(now, $"OFFER rejected: {r}");
        Reply(src, sid, OfferDecision.Rejected, r);
    }

    private static bool SameExceptResume(byte[] a, byte[] b)
    {
        if (a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++)
        {
            byte x = a[i], y = b[i];
            if (i == 8) { x &= 0xFE; y &= 0xFE; }
            if (x != y) return false;
        }
        return true;
    }

    private static bool ResumeMatches(OfferMsg a, OfferMsg b) =>
        a.Target == b.Target && a.Encoding == b.Encoding && a.ChunkSize == b.ChunkSize &&
        a.ImageSize == b.ImageSize && a.StreamSize == b.StreamSize && a.Sha256.AsSpan().SequenceEqual(b.Sha256) &&
        a.StreamCrc32 == b.StreamCrc32 && a.CompanionSha256.AsSpan().SequenceEqual(b.CompanionSha256) &&
        a.CompanionSize == b.CompanionSize;

    private void OnOffer(OfferMsg o, byte[] raw, int src, long now)
    {
        byte sid = o.SessionId;
        if (o.ProtocolVersion != UpdProto.Version) { Reject(src, sid, Reason.UnsupportedProtocol, now); return; }
        if (o.TargetUid != Uid) { Reject(src, sid, Reason.WrongDevice, now); return; }
        if (!Ready) { Reject(src, sid, Reason.NotReady, now); return; }
        if (!AcceptUpdates) { Reject(src, sid, Reason.Disabled, now); return; }

        if (_s is not null)
        {
            var s = _s;
            if (s.Sender == src && s.Sid == sid && SameExceptResume(s.OfferRaw, raw))
            {
                s.LastHeard = now;
                Log(now, "duplicate OFFER");
                AnswerRebind(s, now);
                return;
            }
            if ((o.Flags & OfferFlags.Resume) != 0 && s.State is >= SessionState.Confirming and <= SessionState.Done &&
                ResumeMatches(s.Offer, o))
            {
                // As the firmware: from ANOTHER address only once the bound sender has been
                // silent for more than 5 s (5.3.5); before that it is a second sender (5.8).
                if (src != s.Sender && now - s.LastHeard <= 5_000_000)
                {
                    Reject(src, sid, Reason.Busy, now);
                    return;
                }
                Log(now, $"RESUME: session rebound from {s.Sender}/{s.Sid} to {src}/{sid}");
                s.Sender = src;
                s.Sid = sid;
                s.Offer = o;
                s.OfferRaw = raw;
                s.LastHeard = now;
                if (s.Result is not null) s.Result = s.Result with { SessionId = sid };
                AnswerRebind(s, now);
                return;
            }
            bool active = s.State is not (SessionState.Idle or SessionState.Done or SessionState.Failed);
            if (active && s.Sender == src && now - s.LastHeard > 5_000_000 &&
                s.State is >= SessionState.Confirming and <= SessionState.ReadyToApply)
            {
                Log(now, "pre-empting the sender's own stale session");
                _s = null;
            }
            else if (active)
            {
                Reject(src, sid, Reason.Busy, now);
                return;
            }
        }

        if (OtaState == OtaState.PendingVerify) { Reject(src, sid, Reason.PendingVerify, now); return; }
        if (_restartPending) { Reject(src, sid, Reason.RestartPending, now); return; }
        if (o.Target is not (UpdateTarget.Firmware or UpdateTarget.WebUi)) { Reject(src, sid, Reason.UnsupportedTarget, now); return; }
        if (o.Encoding == StreamEncoding.Deflate && (o.Target == UpdateTarget.WebUi || (Caps & Capabilities.Deflate) == 0) ||
            o.Encoding is not (StreamEncoding.Raw or StreamEncoding.Deflate))
        { Reject(src, sid, Reason.UnsupportedEncoding, now); return; }

        bool bad = sid is 0 or 255 || o.ChunkSize != UpdProto.ChunkSize || o.ImageSize == 0 || o.StreamSize == 0 ||
                   o.StreamSize > UpdProto.MaxStreamSize ||
                   (o.Encoding == StreamEncoding.Raw && o.StreamSize != o.ImageSize) ||
                   (o.Encoding == StreamEncoding.Deflate && o.StreamSize > UpdProto.StreamBound(o.ImageSize)) ||
                   !TextField.IsValidVersion(o.Version) ||
                   (o.Target == UpdateTarget.WebUi && (o.CompanionSize != 0 || o.CompanionSha256.Any(b => b != 0))) ||
                   ((o.CompanionSize != 0) != o.CompanionSha256.Any(b => b != 0)) ||
                   o.CompanionSize > MaxWebUiSize;
        if (bad) { Reject(src, sid, Reason.BadRequest, now); return; }

        uint limit = o.Target == UpdateTarget.Firmware ? MaxFirmwareSize : MaxWebUiSize;
        if (o.ImageSize > limit) { Reject(src, sid, Reason.TooLarge, now); return; }
        if (o.Target == UpdateTarget.Firmware ? o.Board != Board : o.Board is not (BoardId.Any) && o.Board != Board)
        { Reject(src, sid, Reason.WrongBoard, now); return; }
        if (o.Target == UpdateTarget.WebUi && (o.Flags & OfferFlags.AllowVersionMismatch) == 0 && o.Version != RunningVersion)
        { Reject(src, sid, Reason.VersionMismatch, now); return; }
        if (AlarmActive) { Reject(src, sid, Reason.AlarmActive, now); return; }
        if (Script.ForceReject is { } forced) { Reject(src, sid, forced, now); return; }

        var ns = new Sess
        {
            Sender = src, Sid = sid, Offer = o, OfferRaw = raw, LastHeard = now, StateSince = now,
            Stream = new byte[o.StreamSize],
        };
        _s = ns;

        // An approved companion web UI goes in without a dialog (5.3.6).
        if (o.Target == UpdateTarget.WebUi && _companion is { } c && now < _companionUntil &&
            c.Sha.AsSpan().SequenceEqual(o.Sha256) && c.Size == o.ImageSize && c.Version == o.Version)
        {
            ns.ViaCompanion = true;
            Log(now, "web UI OFFER matches the approved companion: accepted without a dialog");
            Accept(ns, now);
            return;
        }
        ns.State = SessionState.Confirming;
        ns.DialogEnd = now + 60_000_000;
        ns.UserAt = Script.User == DisplayScript.UserAction.Ignore ? long.MaxValue : now + Script.UserDelayUs;
        DialogsShown++;
        Log(now, $"dialog: {o.Target} {o.Version} from {o.SenderName} (address {src})" + (o.HasCompanion ? " + web UI" : ""));
        Reply(src, sid, OfferDecision.PendingUser, secs: 60);
    }

    /// <summary>The answers of 5.3.3 / 5.3.4 for a duplicate or a matching RESUME.</summary>
    private void AnswerRebind(Sess s, long now)
    {
        switch (s.State)
        {
            case SessionState.Confirming:
                Reply(s.Sender, s.Sid, OfferDecision.PendingUser, secs: (ushort)Math.Max(0, (s.DialogEnd - now) / 1_000_000));
                break;
            case SessionState.Preparing:
            case SessionState.Verifying:
                Reply(s.Sender, s.Sid, OfferDecision.Accepted, window: UpdProto.MaxWindowChunks);
                Send(s.Sender, BuildStatus(now));
                break;
            case SessionState.Receiving:
            case SessionState.Processing:
                Reply(s.Sender, s.Sid, OfferDecision.Accepted, window: UpdProto.MaxWindowChunks);
                Send(s.Sender, BuildAck(s));
                break;
            case SessionState.ReadyToApply:
            case SessionState.Applying:
            case SessionState.Done:
                Reply(s.Sender, s.Sid, OfferDecision.Accepted, window: UpdProto.MaxWindowChunks);
                if (s.Result is not null) Send(s.Sender, s.Result);
                break;
            default:
                Reply(s.Sender, s.Sid, OfferDecision.Rejected, s.LastReason == Reason.Ok ? Reason.Busy : s.LastReason);
                break;
        }
    }

    private void Accept(Sess s, long now)
    {
        Reply(s.Sender, s.Sid, OfferDecision.Accepted, window: UpdProto.MaxWindowChunks);
        s.State = SessionState.Preparing;
        s.StateSince = now;
        s.Due = now + Script.PrepareUs;
        s.NextStatus = now + 1_000_000;
    }

    // ── DATA (5.4) ────────────────────────────────────────────────────────────
    private void OnData(DataMsg d, int len, int src, long now)
    {
        if (!Bound(src, d.SessionId) || _s!.State is SessionState.Idle or SessionState.Done or SessionState.Failed)
        {
            SessionUnknown(src, d.SessionId, now);
            return;
        }
        var s = _s;
        s.LastHeard = now;
        if (s.State is not (SessionState.Receiving or SessionState.Processing))
        {
            SendA6(s, now);   // answered with STATUS outside the transfer states
            return;
        }
        uint off = d.Offset;
        uint end = s.State == SessionState.Processing ? s.Base : s.End;
        bool aligned = off % UpdProto.ChunkSize == 0;
        bool lenOk = off < s.Offer.StreamSize &&
                     (len == UpdProto.ChunkSize && off + UpdProto.ChunkSize <= s.Offer.StreamSize ||
                      off + len == s.Offer.StreamSize && len <= UpdProto.ChunkSize);
        if (!aligned || !lenOk)
        {
            if (++s.Malformed >= 20)
            {
                Fail(s, Reason.BadRequest, now, sendAbort: true);
            }
            return;
        }
        if (off >= end) { SendA6(s, now); return; }   // A6: no credit for this offset
        if (off < s.Base)
        {
            if (++s.BelowBase >= 3) SendA6(s, now);
            return;
        }
        int bit = (int)((off - s.Base) / UpdProto.ChunkSize);
        if ((s.Bitmap & (1UL << bit)) != 0) return;   // duplicate inside the window
        d.Data.Span.CopyTo(s.Stream.AsSpan((int)off));
        s.Bitmap |= 1UL << bit;
        s.LastData = now;
        s.A4Count = 0;

        int chunks = (int)((s.End - s.Base + UpdProto.ChunkSize - 1) / UpdProto.ChunkSize);
        ulong full = chunks == 64 ? ulong.MaxValue : (1UL << chunks) - 1;
        if (s.Bitmap == full)
        {
            // A1: window complete, no credit while the flash work runs.
            s.Crc = Crc32.Update(s.Crc, s.Stream.AsSpan((int)s.Base, (int)(s.End - s.Base)));
            s.Base = s.End;
            s.Bitmap = 0;
            s.State = SessionState.Processing;
            s.StateSince = now;
            s.Due = now + Script.ProcessingUs;
            s.NextStatus = now + 1_000_000;
            Send(s.Sender, BuildAck(s));
            return;
        }
        if (bit == chunks - 1) Send(s.Sender, BuildAck(s));   // A3: the last chunk came, holes remain
    }

    private void SendA6(Sess s, long now)
    {
        if (now - s.LastA6 < 250_000) return;
        s.LastA6 = now;
        s.BelowBase = 0;
        Send(s.Sender, s.State is SessionState.Receiving or SessionState.Processing ? BuildAck(s) : BuildStatus(now));
    }

    // ── APPLY, ABORT (5.6) ────────────────────────────────────────────────────
    private void OnApply(ApplyMsg a, int src, long now)
    {
        if (!Bound(src, a.SessionId)) { SessionUnknown(src, a.SessionId, now); return; }
        var s = _s!;
        s.LastHeard = now;
        if (s.State == SessionState.Applying) return;
        if (s.State == SessionState.Done) { if (s.Result is not null) Send(src, s.Result); return; }
        if (s.State != SessionState.ReadyToApply) return;
        s.State = SessionState.Applying;
        s.StateSince = now;
        s.Due = now + Script.ApplyUs;
        s.NextStatus = now + 1_000_000;
        s.ApplyMode = a.Mode;
        Log(now, $"APPLY mode {a.Mode}");
    }

    private void OnAbort(AbortMsg a, int src, long now)
    {
        if (!Bound(src, a.SessionId)) return;
        var s = _s!;
        if (s.State is SessionState.Idle or SessionState.Applying or SessionState.Done) return;
        Log(now, $"ABORT from the sender: {a.Reason}");
        if (s.State == SessionState.Confirming) { _s = null; return; }
        s.State = SessionState.Failed;
        s.LastReason = a.Reason;
        s.StateSince = now;
    }

    private void Fail(Sess s, Reason r, long now, bool sendAbort)
    {
        Log(now, $"session failed: {r}");
        if (sendAbort) Send(s.Sender, new AbortMsg { SessionId = s.Sid, Reason = r });
        s.State = SessionState.Failed;
        s.LastReason = r;
        s.StateSince = now;
    }

    // ── Timers ────────────────────────────────────────────────────────────────
    private long TickSession(long now)
    {
        var s = _s;
        if (s is null) return long.MaxValue;
        long next = long.MaxValue;
        switch (s.State)
        {
            case SessionState.Confirming:
                if (now - s.LastHeard > 15_000_000) { Log(now, "sender silent for 15 s: dialog closed"); _s = null; return long.MaxValue; }
                if (AlarmActive) { Reject(s.Sender, s.Sid, Reason.AlarmActive, now); _s = null; return long.MaxValue; }
                if (now >= s.UserAt)
                {
                    if (Script.User == DisplayScript.UserAction.Accept) { Log(now, "user installs"); Accept(s, now); return now; }
                    Log(now, "user declines");
                    Reply(s.Sender, s.Sid, OfferDecision.Rejected, Reason.DeclinedByUser);
                    _s = null;
                    return long.MaxValue;
                }
                if (now >= s.DialogEnd)
                {
                    Reply(s.Sender, s.Sid, OfferDecision.Rejected, Reason.ConfirmTimeout);
                    _s = null;
                    return long.MaxValue;
                }
                return Math.Min(Math.Min(s.UserAt, s.DialogEnd), s.LastHeard + 15_000_001);

            case SessionState.Preparing:
                if (now >= s.Due)
                {
                    s.State = SessionState.Receiving;
                    s.StateSince = now;
                    s.Base = 0;
                    s.End = Math.Min((uint)(UpdProto.MaxWindowChunks * UpdProto.ChunkSize), s.Offer.StreamSize);
                    s.LastData = now;
                    Send(s.Sender, BuildAck(s));
                    return now + 1_000_000;
                }
                next = Math.Min(s.Due, KeepAlive(s, now));
                break;

            case SessionState.Receiving:
                if (now - s.LastHeard > 60_000_000) { Fail(s, Reason.Timeout, now, sendAbort: true); return now; }
                if (now - s.LastData >= 1_000_000 && s.A4Count < 5)
                {
                    // A4: no DATA for 1000 ms while the window is incomplete.
                    s.A4Count++;
                    s.LastData = now;
                    Send(s.Sender, BuildAck(s));
                }
                next = s.A4Count < 5 ? s.LastData + 1_000_000 : s.LastHeard + 60_000_001;
                break;

            case SessionState.Processing:
                if (now - s.LastHeard > 60_000_000) { Fail(s, Reason.Timeout, now, sendAbort: true); return now; }
                if (now >= s.Due)
                {
                    double fraction = (double)s.Base / s.Offer.StreamSize;
                    if (Script.RestartAtFraction is { } rf && fraction >= rf)
                    {
                        Script.RestartAtFraction = null;
                        Reboot(now, "power cycle during the transfer");
                        return _bootUs + Script.BootUs;
                    }
                    if (Script.CancelAtFraction is { } cf && fraction >= cf)
                    {
                        Script.CancelAtFraction = null;
                        Fail(s, Reason.UserCancelled, now, sendAbort: true);
                        return now;
                    }
                    if (s.Base >= s.Offer.StreamSize)
                    {
                        s.State = SessionState.Verifying;
                        s.StateSince = now;
                        s.Due = now + Script.VerifyUs;
                        s.NextStatus = now;
                        return now;
                    }
                    // A2: processing finished, credit for the next window.
                    s.State = SessionState.Receiving;
                    s.StateSince = now;
                    s.End = Math.Min(s.Base + (uint)(UpdProto.MaxWindowChunks * UpdProto.ChunkSize), s.Offer.StreamSize);
                    s.LastData = now;
                    s.A4Count = 0;
                    Send(s.Sender, BuildAck(s));
                    return now + 1_000_000;
                }
                next = Math.Min(s.Due, KeepAlive(s, now));
                break;

            case SessionState.Verifying:
                if (now >= s.Due) { Verify(s, now); return now; }
                next = Math.Min(s.Due, KeepAlive(s, now));
                break;

            case SessionState.ReadyToApply:
                if (now - s.StateSince > 600_000_000) { Log(now, "no APPLY within 10 min: discarded"); _s = null; return long.MaxValue; }
                next = RepeatResult(s, now);
                break;

            case SessionState.Applying:
                if (now >= s.Due) { DoApply(s, now); return now; }
                next = Math.Min(s.Due, KeepAlive(s, now));
                break;

            case SessionState.Done:
            case SessionState.Failed:
                if (now - s.StateSince > 30_000_000) { _s = null; return long.MaxValue; }
                next = Math.Min(s.StateSince + 30_000_001, s.State == SessionState.Done ? RepeatResult(s, now) : long.MaxValue);
                break;
        }
        return next;
    }

    private long KeepAlive(Sess s, long now)
    {
        if (now >= s.NextStatus)
        {
            Send(s.Sender, BuildStatus(now));
            s.NextStatus = now + 1_000_000;
        }
        return s.NextStatus;
    }

    /// <summary>RESULT every 5 s in READY_TO_APPLY and DONE while the sender was heard within 60 s (3.7).</summary>
    private long RepeatResult(Sess s, long now)
    {
        if (s.Result is null || now - s.LastHeard > 60_000_000) return long.MaxValue;
        if (now - s.LastResult >= 5_000_000)
        {
            s.LastResult = now;
            Send(s.Sender, s.Result);
        }
        return s.LastResult + 5_000_000;
    }

    private void Verify(Sess s, long now)
    {
        var o = s.Offer;
        Reason code = Reason.Ok;
        byte[] decoded = s.Stream;
        if (Crc32.Compute(s.Stream) != o.StreamCrc32) code = Reason.StreamCrcMismatch;
        else if (o.Encoding == StreamEncoding.Deflate)
        {
            try { decoded = DeflateCache.Inflate(s.Stream, (int)o.ImageSize); }
            catch (InvalidDataException) { code = Reason.SizeMismatch; }
            if (code == Reason.Ok && decoded.Length != o.ImageSize) code = Reason.SizeMismatch;
        }
        if (code == Reason.Ok && Script.CorruptFlash)
        {
            decoded = (byte[])decoded.Clone();
            decoded[decoded.Length / 2] ^= 0x40;
        }
        byte[] sha = code == Reason.Ok ? SHA256.HashData(decoded) : new byte[32];
        if (code == Reason.Ok && !sha.AsSpan().SequenceEqual(o.Sha256)) code = Reason.ShaMismatch;
        if (code == Reason.Ok && o.Target == UpdateTarget.Firmware)
        {
            if (!FwDescriptor.HasMagic(decoded)) code = Reason.DescriptorInvalid;
            else
            {
                var d = FwDescriptor.Parse(decoded.AsSpan(FwDescriptor.Offset, FwDescriptor.Size));
                var panel = Boards.Panel(Board);
                if (d.Board != Board || panel is null || d.PanelWidth != panel.Value.W || d.PanelHeight != panel.Value.H) code = Reason.WrongBoard;
                else if (!d.OtaGuard || d.FwVersion != o.Version) code = Reason.DescriptorInvalid;
            }
        }
        if (code == Reason.Ok && o.Target == UpdateTarget.WebUi && (decoded.Length < 3 || decoded[0] != 0x1F || decoded[1] != 0x8B || decoded[2] != 0x08))
            code = Reason.ImageInvalid;

        s.Result = new ResultMsg { SessionId = s.Sid, Code = code, Phase = ResultPhase.Verified, Sha256 = sha };
        s.LastResult = now;
        Send(s.Sender, s.Result);
        Log(now, $"verified: {code}");
        if (code == Reason.Ok)
        {
            s.Decoded = decoded;
            LastWrittenImage = decoded;
            s.State = SessionState.ReadyToApply;
            s.StateSince = now;
        }
        else
        {
            s.State = SessionState.Failed;
            s.LastReason = code;
            s.StateSince = now;
        }
    }

    private void DoApply(Sess s, long now)
    {
        var mode = s.ApplyMode;
        var o = s.Offer;
        if (o.Target == UpdateTarget.Firmware)
        {
            int other = 1 - RunningSlot;
            var img = s.Decoded!;
            _slots[other] = new Slot { Id = ImageId.Read(img.AsSpan(FirmwareImage.ImageIdOffset)), Version = o.Version, Bytes = img };
            _bootSlot = other;
            _h5 = (_slots[other].Id, (byte)(mode == ApplyWireMode.Now ? 0 : 1));
            if (o.HasCompanion) _companion = (o.CompanionSha256, o.CompanionSize, _slots[other].Id, o.Version);
            byte detail = mode == ApplyWireMode.NextRestart ? (byte)1 : (byte)0;
            s.Result = new ResultMsg { SessionId = s.Sid, Code = Reason.Ok, Phase = ResultPhase.Activated, Detail = detail, Sha256 = s.Result!.Sha256 };
            if (mode == ApplyWireMode.NextRestart) _restartPending = true;
            else
            {
                switch (Script.Restart)
                {
                    case DisplayScript.RestartChoice.RestartNow: _restartAt = now + 500_000; break;
                    case DisplayScript.RestartChoice.Later:
                        _restartPending = true;
                        _h5 = (_slots[other].Id, 1);
                        Log(now, "user chose Later");
                        break;
                    default: _restartAt = now + 10_000_000; break;
                }
                if (Script.Restart != DisplayScript.RestartChoice.Later) _restartPending = true;
            }
        }
        else
        {
            SetWebUi(s.Decoded!, o.Version);
            if (s.ViaCompanion) { _companion = null; _companionUntil = long.MinValue; }
            s.Result = new ResultMsg { SessionId = s.Sid, Code = Reason.Ok, Phase = ResultPhase.Applied, Detail = 0, Sha256 = s.Result!.Sha256 };
        }
        s.State = SessionState.Done;
        s.StateSince = now;
        s.LastResult = now;
        Send(s.Sender, s.Result);
        Log(now, $"applied: {s.Result.Phase}");
    }

    // ── Restart, OtaGuard (10.2) ──────────────────────────────────────────────
    private void Reboot(long now, string why)
    {
        Restarts++;
        Log(now, "restart: " + why);
        _s = null;
        _restartAt = long.MaxValue;
        _bootUs = now;
        _booting = true;
        _claimSent = false;
        _protoSinceBoot = _anySinceBoot = false;
        _asm.Clear();
        _rx.Clear();
        if (Script.AddressAfterBoot is { } a) { Address = a; Script.AddressAfterBoot = null; }
        if (_bootSlot != RunningSlot)
        {
            RunningSlot = _bootSlot;
            OtaState = OtaState.PendingVerify;
            _restartPending = false;
        }
        else _restartPending = false;
        // The companion approval only survives into the boot of the image it names (5.3.6).
        if (_companion is { } c && c.FwId != _slots[RunningSlot].Id) _companion = null;
        _companionUntil = long.MinValue;
    }

    private long SetupDoneUs => _bootUs + Script.BootUs;

    private long TickGuard(long now)
    {
        if (OtaState != OtaState.PendingVerify) return long.MaxValue;
        long setup = SetupDoneUs;
        if (Script.FailHealthCriterion is { } crit && crit != 5 && now >= setup + 15_000_000)
        {
            RollBack(now, crit);
            return now;
        }
        bool h5 = _h5 is not { } m || m.Id != RunningImageId || (m.Mode == 0 ? _protoSinceBoot : _anySinceBoot);
        if (Script.FailHealthCriterion == 5) h5 = false;
        if (now >= setup + 60_000_000 && h5)
        {
            OtaState = OtaState.Valid;
            _h5 = null;
            Log(now, $"marked valid: {RunningVersion}");
            if (_companion is { } c && c.FwId == RunningImageId) _companionUntil = now + 30 * 60_000_000L;
            return long.MaxValue;
        }
        if (now >= setup + 180_000_000)
        {
            RollBack(now, 5);
            return now;
        }
        long check = now < setup + 60_000_000 ? setup + 60_000_000 : now + 1_000_000;
        return Math.Min(check, setup + 180_000_000);
    }

    private void RollBack(long now, byte detail)
    {
        var failed = _slots[RunningSlot];
        Log(now, $"rollback: health criterion {detail}");
        _rbInfo = (failed.Version, failed.Id, RollbackCause.HealthCheck, detail);
        _bootSlot = 1 - RunningSlot;
        _companion = null;
        Reboot(now, "rollback");
        OtaState = OtaState.Valid;
        Script.FailHealthCriterion = null;
    }

    // ── Message builders ──────────────────────────────────────────────────────
    private InfoMsg BuildInfo(long now)
    {
        var flags = StatusFlags.Ready;
        if (AcceptUpdates) flags |= StatusFlags.AcceptUpdates;
        if (AlarmActive) flags |= StatusFlags.AlarmActive;
        if (_restartPending) flags |= StatusFlags.RestartPending;
        bool rolledBack = _rbInfo is { } rb && _slots[1 - RunningSlot].Id == rb.Id;
        if (rolledBack) flags |= StatusFlags.RolledBack;
        if (_companion is not null && now < _companionUntil) flags |= StatusFlags.CompanionApproved;
        if (!Ready) flags &= ~StatusFlags.Ready;
        ushort verifyLeft = 0xFFFF;
        if (OtaState == OtaState.PendingVerify)
            verifyLeft = (ushort)Math.Clamp((SetupDoneUs + 180_000_000 - now) / 1_000_000, 0, 180);
        return new InfoMsg
        {
            SessionId = _s?.Sid ?? 0,
            Board = Board,
            Caps = Caps,
            Uid = Uid,
            SessionState = _s?.State ?? SessionState.Idle,
            OtaState = OtaState,
            VerifySecondsLeft = verifyLeft,
            RunningSlot = (byte)RunningSlot,
            Flags = flags,
            MaxFirmwareSize = MaxFirmwareSize,
            MaxWebUiSize = MaxWebUiSize,
            RunningVersion = RunningVersion,
            LastInvalidVersion = rolledBack ? _rbInfo!.Value.Version : "",
            Label = Label,
            RunningImageId = RunningImageId,
            LastInvalidImageId = rolledBack ? _rbInfo!.Value.Id : ImageId.None,
            RollbackCause = rolledBack ? _rbInfo!.Value.Cause : RollbackCause.None,
            RollbackDetail = rolledBack ? _rbInfo!.Value.Detail : (byte)0,
            WebUiVersion = WebUiVersion,
            WebUiId = WebUiId,
            WebUiSize = WebUiSize,
            UptimeSeconds = Uptime(now),
        };
    }

    private StatusMsg BuildStatus(long now)
    {
        var s = _s;
        if (s is null) return new StatusMsg { SessionId = 0, State = SessionState.Idle, Seconds = 0xFFFF };
        ushort secs = s.State switch
        {
            SessionState.Confirming => (ushort)Math.Max(0, (s.DialogEnd - now) / 1_000_000),
            SessionState.Verifying => (ushort)Math.Max(0, (s.Due - now) / 1_000_000),
            SessionState.ReadyToApply => (ushort)Math.Max(0, (s.StateSince + 600_000_000 - now) / 1_000_000),
            _ => 0xFFFF,
        };
        uint written = s.Offer.Encoding == StreamEncoding.Raw ? s.Base
            : (uint)Math.Min(s.Offer.ImageSize, (double)s.Base / s.Offer.StreamSize * s.Offer.ImageSize);
        if (s.State >= SessionState.Verifying && s.State != SessionState.Failed) written = s.Offer.ImageSize;
        return new StatusMsg
        {
            SessionId = s.Sid, State = s.State, Reason = s.LastReason,
            StreamAccepted = s.Base, StreamSize = s.Offer.StreamSize,
            ImageWritten = written, ImageSize = s.Offer.ImageSize, Seconds = secs,
        };
    }

    private AckMsg BuildAck(Sess s) => new()
    {
        SessionId = s.Sid,
        State = s.State == SessionState.Processing ? SessionState.Processing : SessionState.Receiving,
        Base = s.Base,
        End = s.State == SessionState.Processing ? s.Base : s.End,
        Bitmap = s.State == SessionState.Processing ? 0 : s.Bitmap,
        StreamCrc32 = s.Base == 0 ? 0 : s.Crc,
        HoldMs = s.State == SessionState.Processing ? Script.HoldMs : (ushort)0,
    };

    // ── Sending ───────────────────────────────────────────────────────────────
    private void Send(int dst, UpdateMessage m)
    {
        if (_booting || !_link.IsOpen) return;
        uint id = CanId.Encode(UpdProto.PrioControl, UpdProto.Pgn, Address, dst);
        _link.SendBurst(FastPacket.Encode(id, m.Encode(), _seq.Take(Address, UpdProto.Pgn)));
    }

    private void SendPgn(int pgn, int dst, byte[] payload)
    {
        uint id = CanId.Encode(6, pgn, Address, dst);
        if (_link.IsOpen) _link.SendBurst(FastPacket.Encode(id, payload, _seq.Take(Address, pgn)));
    }

    private void SendRaw(uint id, byte[] data)
    {
        if (_link.IsOpen) _link.SendBurst([new CanFrame(id, data)]);
    }
}
