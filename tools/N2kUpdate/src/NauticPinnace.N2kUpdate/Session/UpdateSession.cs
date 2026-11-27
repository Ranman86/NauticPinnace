using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Session;

/// <summary>
/// One transfer of one image to one display: the sender state machine of 5.10, from
/// OFFER to the outcome of 5.7. Tick-driven and single-threaded (engine thread).
/// </summary>
public sealed class UpdateSession
{
    private enum St { Offering, Confirming, WaitCredit, Sending, WaitAck, WaitResult, Applying, WaitReboot, WaitWebUi, Recover, Diagnose, Done }
    private enum Rec { Reopen, Rediscover, ResumeOffer }

    private const int Chunk = UpdProto.ChunkSize;

    private readonly N2kEngine _eng;
    private readonly DisplayDevice _dev;
    private readonly TransferImage _img;
    private readonly WebUiImage? _companion;
    private readonly UpdateOptions _opt;
    private readonly SessionTimers _t;
    private readonly byte _sid;
    private readonly Action<UpdateProgress>? _progress;
    private readonly PrefixCrc _crc;
    private readonly int[] _sends;
    private readonly Queue<int> _queue = new();
    private readonly List<(long Us, uint Base)> _rate = new();

    private St _st = St.Offering;
    private bool _resume;
    private bool _offerSent;
    private bool _accepted;            // the display accepted this session (ACCEPTED, or an ACK/STATUS beyond the dialog)
    private bool _remembered;          // recorded in the engine's SessionMemory
    private bool _checkBaseAfterResume;
    private Reason _recoverReject;     // the last rejection of an OFFER RESUME during RECOVER
    private int _attempts;
    private long _deadline;
    private long _nextAction;
    private long _lastHeardUs;
    private long _lastProgressUs = long.MinValue / 2;

    // Transfer.
    private uint _highestBase;
    private bool _holdMode;
    private long _holdUs;
    private int _ackQueries;
    private int _pendingChunk = -1;
    private byte[]? _pendingPayload;
    private bool _paused;
    private long _pausedSince;
    private long _dialogEndUs = -1;
    private long _resultDeadline;
    private bool _askResult;

    // After APPLY.
    private byte _sOld = 0xFF;
    private ImageId _idNew;
    private long _tApply;
    private long _tApplyFirst;         // the first APPLY: T_apply when no RESULT(phase 3) came
    private long _lastInfoUs = long.MinValue / 2;
    private InfoMsg? _lastInfo;

    // Restart detection: the display's uptime at a known moment.
    private uint _refUptime;
    private long _refUs;
    private bool _refTimed;

    // Recovery.
    private Rec _rec;
    private long _recStart;
    private long _recDeadline;
    private int _recRounds;

    public UpdateSession(N2kEngine engine, DisplayDevice device, TransferImage image, UpdateOptions options,
                         WebUiImage? companion, byte sessionId, bool resume, Action<UpdateProgress>? progress)
    {
        _eng = engine;
        _dev = device;
        _img = image;
        _opt = options;
        _t = options.Timers;
        _companion = image.Target == UpdateTarget.Firmware ? companion : null;
        _sid = sessionId;
        _resume = resume;
        _progress = progress;
        _crc = new PrefixCrc(image.Stream);
        _sends = new int[image.ChunkCount];
        _refUptime = device.Info.UptimeSeconds;
        _refUs = device.InfoAtUs;
        _refTimed = device.InfoAtUs > 0;
    }

    public UpdateOutcome? Outcome { get; private set; }
    public bool IsFinished => Outcome is not null;
    public byte SessionId => _sid;
    public UpdateTarget Target => _img.Target;
    public bool CompanionOffered => _companion is not null;

    /// <summary>The latest INFO of the display seen by this session.</summary>
    public InfoMsg LatestInfo => _lastInfo ?? _dev.Info;

    /// <summary>Chunks sent more than once (for the log).</summary>
    public int Resends { get; private set; }

    public string StateName => _st.ToString();

    private bool IsFirmware => _img.Target == UpdateTarget.Firmware;

    private ApplyWireMode WireMode => _opt.ApplyMode switch
    {
        ApplyMode.NextRestart => ApplyWireMode.NextRestart,
        ApplyMode.NowNoWait   => ApplyWireMode.NowNoWait,
        _                     => ApplyWireMode.Now,
    };

    // ── Start ─────────────────────────────────────────────────────────────────
    public void Begin(long now)
    {
        _lastHeardUs = now;
        _eng.Info($"{TargetName}: offering {_img.Version} to {_dev} (session {_sid}, {(_img.Encoding == StreamEncoding.Deflate ? "deflate" : "raw")}, " +
                  $"image {_img.ImageSize} B, stream {_img.StreamSize} B{(_companion is not null ? ", with companion web UI" : "")}{(_resume ? ", RESUME" : "")})");
        EnterOffering(now, _resume);
    }

    private string TargetName => IsFirmware ? "firmware" : "web UI";

    private OfferMsg BuildOffer(bool resume)
    {
        var flags = OfferFlags.None;
        if (resume) flags |= OfferFlags.Resume;
        if (!IsFirmware && _opt.AllowWebUiVersionMismatch) flags |= OfferFlags.AllowVersionMismatch;
        return new OfferMsg
        {
            SessionId = _sid,
            Target = _img.Target,
            Board = IsFirmware ? _img.Board : BoardId.Any,
            Encoding = _img.Encoding,
            Flags = flags,
            ImageSize = _img.ImageSize,
            StreamSize = _img.StreamSize,
            Sha256 = _img.Sha256,
            TargetUid = _dev.Uid,
            Version = _img.Version,
            SenderName = _opt.SenderName,
            SenderKind = _opt.SenderKind,
            EstimatedSeconds = EstimateSeconds(),
            StreamCrc32 = _img.StreamCrc32,
            CompanionSha256 = _companion?.Sha256 ?? new byte[32],
            CompanionSize = (uint)(_companion?.Size ?? 0),
        };
    }

    /// <summary>The transfer-time estimate of OFFER byte 89: DATA bits at the configured share plus window work.</summary>
    private ushort EstimateSeconds()
    {
        double frames = (double)_img.StreamSize / Chunk * 32;
        double bits = frames * 137;
        double share = _eng.Limiter.ConfiguredShare;
        double windows = Math.Ceiling((double)_img.StreamSize / (Chunk * UpdProto.MaxWindowChunks));
        double s = bits / (share * 250_000) + windows * 0.3;
        return (ushort)Math.Clamp(Math.Ceiling(s), 1, 0xFFFE);
    }

    private void EnterOffering(long now, bool resume)
    {
        _st = St.Offering;
        _resume = resume;
        _attempts = 0;
        SendOffer(now);
        Report(now, SenderPhase.Offering, force: true);
    }

    private void SendOffer(long now)
    {
        _attempts++;
        _offerSent = true;
        _eng.SendControl(_dev.Address, BuildOffer(_resume));
        _deadline = now + _t.OfferReplyUs;
    }

    // ── Incoming ──────────────────────────────────────────────────────────────
    public void OnMessage(UpdateMessage msg, int src, long now)
    {
        if (_st == St.Done) return;
        if (!UpdateMessage.IsDisplayToSender(msg.Type)) return;
        if (msg.ProtocolVersion != UpdProto.Version) return;

        if (msg is InfoMsg info)
        {
            if (info.Uid != _dev.Uid) return;
            HandleInfo(info, src, now);
            return;
        }
        if (src != _dev.Address || msg.SessionId != _sid) return;
        _lastHeardUs = now;
        if (_st == St.WaitResult) _nextAction = Math.Max(_nextAction, now + _t.ResultQuerySilenceUs);

        switch (msg)
        {
            case OfferReplyMsg r: HandleOfferReply(r, now); break;
            case AckMsg a:        HandleAck(a, now); break;
            case StatusMsg s:     HandleStatus(s, now); break;
            case ResultMsg r:     HandleResult(r, now); break;
            case AbortMsg a:      HandleAbort(a, now); break;
        }
    }

    private bool InOfferPhase => _st is St.Offering or St.Confirming || (_st == St.Recover && _rec == Rec.ResumeOffer);
    private bool InTransfer => _st is St.WaitCredit or St.Sending or St.WaitAck;

    private void HandleOfferReply(OfferReplyMsg r, long now)
    {
        if (!InOfferPhase) return;
        bool recovering = _st == St.Recover;
        switch (r.Decision)
        {
            case OfferDecision.PendingUser:
                if (_accepted)
                {
                    // A RESUME answered with a dialog, after the display had accepted this
                    // session: it no longer holds it (restarted, or timed out) and took the
                    // OFFER for a new one. That dialog belongs to no transfer: close it.
                    SendAbort(Reason.SenderCancelled);
                    Finish(now, new UpdateOutcome
                    {
                        Kind = OutcomeKind.Failed, Reason = Reason.SessionUnknown, ByDevice = true, Note = SenderNote.SessionDropped,
                        Message = "the display no longer held the session; the dialog it opened for the RESUME was closed",
                    });
                    return;
                }
                RememberSession();
                if (_st != St.Confirming)
                {
                    _st = St.Confirming;
                    _deadline = now + r.Seconds * 1_000_000L + _t.ConfirmExtraUs;
                    _nextAction = now + _t.ConfirmKeepaliveUs;
                    _eng.Info($"{TargetName}: waiting for confirmation on the display ({r.Seconds} s)");
                }
                _dialogEndUs = now + r.Seconds * 1_000_000L;
                Report(now, SenderPhase.WaitingForConfirmation, force: true);
                break;
            case OfferDecision.Accepted:
                _eng.Info($"{TargetName}: accepted by the display" + (_st == St.Offering && !_resume ? " (approved companion, no dialog)" : ""));
                // After a RESUME the first ACK must not start below what this session
                // had reached (a fresh session of a restarted display would).
                if (recovering || _resume) _checkBaseAfterResume = _highestBase > 0;
                EnterWaitCredit(now);
                break;
            case OfferDecision.Rejected:
                if (recovering && r.Reason == Reason.Busy && now < _recDeadline)
                {
                    // From a new address the display lets a session go only once its old
                    // sender has been silent for 5 s (5.3.4, as 5.3.5): ask again shortly.
                    _recoverReject = r.Reason;
                    _attempts = 0;
                    _nextAction = now + _t.OfferReplyUs;
                    _eng.Info($"{TargetName}: RESUME refused as BUSY, trying again");
                    return;
                }
                if (recovering && r.Reason == Reason.NotReady)
                {
                    // The display is starting up: whatever it held is gone.
                    Finish(now, new UpdateOutcome
                    {
                        Kind = OutcomeKind.Failed, Reason = Reason.SessionUnknown, ByDevice = true, Note = SenderNote.DisplayRestarted,
                        Message = "the display restarted during the transfer",
                    });
                    return;
                }
                Finish(now, new UpdateOutcome { Kind = OutcomeKind.Rejected, Reason = r.Reason, ByDevice = true, Info = _dev.Info });
                break;
        }
    }

    private void EnterWaitCredit(long now)
    {
        _st = St.WaitCredit;
        _holdMode = false;
        _ackQueries = 0;
        _deadline = now + _t.FirstCreditUs;
        _accepted = true;
        RememberSession();
        Report(now, SenderPhase.Preparing, force: true);
    }

    /// <summary>This session is one the engine may RESUME later - also as a new run (5.3.4).</summary>
    private void RememberSession()
    {
        if (_remembered) return;
        _remembered = true;
        _eng.Sessions.Remember(_opt.CacheDirectory, SessionMemory.For(_dev.Uid, _sid, _img));
    }

    private void HandleAck(AckMsg a, long now)
    {
        if (InOfferPhase)
        {
            // An ACK of our session means the user accepted and the reply was lost (5.10).
            if (a.State < SessionState.Preparing) return;
            EnterWaitCredit(now);
        }
        if (!InTransfer && _st != St.WaitResult) return;
        if (_checkBaseAfterResume)
        {
            _checkBaseAfterResume = false;
            if (a.Base < _highestBase)
            {
                // The first ACK after a RESUME starts below what this session had reached:
                // the display is running a fresh session, not this one (a restart and a
                // new dialog). Never feed it the old stream from the middle.
                SendAbort(Reason.SenderCancelled);
                Finish(now, new UpdateOutcome
                {
                    Kind = OutcomeKind.Failed, Reason = Reason.SessionUnknown, ByDevice = true, Note = SenderNote.DisplayRestarted,
                    Message = $"after the RESUME the display was at {a.Base} of {_highestBase} bytes: it restarted during the transfer",
                });
                return;
            }
        }
        if (a.Base < _highestBase) return;   // stale (3.6)

        uint size = _img.StreamSize;
        if (a.End < a.Base || a.End > size || a.End - a.Base > UpdProto.MaxWindowChunks * Chunk ||
            (a.Base % Chunk != 0 && a.Base != size))
        {
            _eng.Info($"{TargetName}: ignoring a malformed ACK (base {a.Base}, end {a.End})");
            return;
        }
        uint ours = _crc.Get(a.Base);
        if (a.Base > 0 && ours != a.StreamCrc32)
        {
            _eng.Info($"{TargetName}: stream CRC mismatch at {a.Base}: display 0x{a.StreamCrc32:X8}, sender 0x{ours:X8}");
            SendAbort(Reason.StreamCrcMismatch);
            Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.StreamCrcMismatch });
            return;
        }
        if (a.Base > _highestBase) _rate.Add((now, a.Base));
        _highestBase = a.Base;

        if (_st == St.WaitResult) return;
        if (a.Base == size) { EnterWaitResult(now); return; }

        if (a.HasCredit)
        {
            _queue.Clear();
            int first = (int)(a.Base / Chunk);
            int count = (int)((a.End - a.Base + Chunk - 1) / Chunk);
            for (int i = 0; i < count; i++)
                if ((a.Bitmap & (1UL << i)) == 0) _queue.Enqueue(first + i);
            _ackQueries = 0;
            if (_queue.Count > 0)
            {
                _st = St.Sending;
                Report(now, SenderPhase.Transferring, force: false);
            }
            else
            {
                // Every bit set but the window not complete: only a QUERY(ACK) can tell.
                _st = St.WaitAck;
                _deadline = now + _t.AckAfterQueueUs;
            }
        }
        else
        {
            // No credit: the display is processing the window (5.4 step 5).
            _st = St.WaitCredit;
            _holdMode = true;
            _ackQueries = 0;
            _holdUs = a.HoldMs == 0xFFFF ? _t.HoldUnknownUs : a.HoldMs * 1000L;
            _deadline = now + _holdUs + _t.HoldExtraUs;
            Report(now, SenderPhase.Transferring, force: false);
        }
    }

    private void HandleStatus(StatusMsg s, long now)
    {
        if (_st == St.Confirming || InOfferPhase)
        {
            if (s.State == SessionState.Confirming)
            {
                if (s.Seconds != 0xFFFF) _dialogEndUs = now + s.Seconds * 1_000_000L;
                Report(now, SenderPhase.WaitingForConfirmation, force: true);
                return;
            }
            if (s.State is >= SessionState.Preparing and <= SessionState.Done) { EnterWaitCredit(now); }
            else if (s.State is SessionState.Failed or SessionState.Idle)
            {
                Finish(now, s.Reason != Reason.Ok
                    ? new UpdateOutcome { Kind = OutcomeKind.Rejected, Reason = s.Reason, ByDevice = true }
                    : new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.Ok, Note = SenderNote.DialogClosed, Message = "the display closed the dialog" });
                return;
            }
            else return;
        }

        switch (s.State)
        {
            case SessionState.Failed:
                Finish(now, new UpdateOutcome { Kind = _st == St.WaitResult ? OutcomeKind.Failed : OutcomeKind.Aborted, Reason = s.Reason, ByDevice = true });
                return;
            case SessionState.Idle:
                if (_st is St.Applying or St.WaitReboot or St.WaitWebUi) return;
                EnterDiagnose(now);
                return;
        }

        if (InTransfer)
        {
            switch (s.State)
            {
                case SessionState.Preparing:
                case SessionState.Processing:
                    // Every STATUS restarts the credit timer (5.4 step 5, 5.10 WAIT_CREDIT).
                    if (_st == St.WaitAck || (_st == St.WaitCredit && !_holdMode && s.State == SessionState.Processing))
                    {
                        // The A1 ACK was lost; its hold is unknown.
                        _st = St.WaitCredit;
                        _holdMode = true;
                        _holdUs = _t.HoldUnknownUs;
                    }
                    if (_st == St.WaitCredit)
                    {
                        _ackQueries = 0;
                        _deadline = now + (_holdMode ? _holdUs + _t.HoldExtraUs : _t.FirstCreditUs);
                    }
                    if (s.State == SessionState.Preparing && s.Seconds != 0xFFFF)
                        Report(now, SenderPhase.Preparing, force: false, message: $"display preparing ({s.Seconds} s)");
                    break;
                case SessionState.Verifying:
                    EnterWaitResult(now);
                    break;
                case SessionState.ReadyToApply:
                case SessionState.Applying:
                case SessionState.Done:
                    EnterWaitResult(now);
                    _nextAction = now;   // ask for the stored RESULT at once
                    break;
            }
            return;
        }

        if (_st == St.WaitResult)
        {
            _resultDeadline = now + _t.ResultUs;
            if (s.State is SessionState.ReadyToApply or SessionState.Applying or SessionState.Done) _nextAction = now;
            else _nextAction = now + _t.ResultQuerySilenceUs;
            Report(now, SenderPhase.Verifying, force: false);
        }
        else if (_st == St.Applying && s.State == SessionState.Done)
        {
            _askResult = true;   // the RESULT was lost; it repeats every 5 s, but ask
        }
    }

    private void HandleResult(ResultMsg r, long now)
    {
        if (r.Phase == ResultPhase.Verified)
        {
            if (!(InTransfer || _st == St.WaitResult || InOfferPhase)) return;
            if (r.Code != Reason.Ok)
            {
                Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = r.Code, ByDevice = true, Message = "verification on the display failed" });
                return;
            }
            if (!r.Sha256.AsSpan().SequenceEqual(_img.Sha256))
            {
                // Can only happen through a display bug; never apply it (5.5).
                SendAbort(Reason.ShaMismatch);
                Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.ShaMismatch, Message = "the display's SHA-256 differs from the file" });
                return;
            }
            _highestBase = _img.StreamSize;
            _eng.Info($"{TargetName}: verified by the display (SHA-256 {Convert.ToHexString(r.Sha256)[..16]}...)");
            if (_opt.ApplyMode == ApplyMode.None)
            {
                Finish(now, UpdateOutcome.Of(OutcomeKind.Verified, SenderNote.NotActivated, "transferred and verified, not activated"));
                return;
            }
            EnterApplying(now);
            return;
        }

        // Phase 2 or 3: the image was applied (our APPLY, or a RESUME after it).
        if (!(_st == St.Applying || InTransfer || _st == St.WaitResult || InOfferPhase)) return;
        if (r.Code != Reason.Ok)
        {
            Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = r.Code, ByDevice = true, Message = "activation failed" });
            return;
        }
        if (_sOld == 0xFF) { _sOld = _dev.Info.RunningSlot; _idNew = _img.Id; }
        if (IsFirmware && r.Phase == ResultPhase.Activated)
        {
            _tApply = now;
            if (_opt.ApplyMode == ApplyMode.Now) EnterWaitReboot(now);
            else Finish(now, new UpdateOutcome
            {
                Kind = OutcomeKind.InstalledNotConfirmed,
                Detail = r.Detail,
                Note = r.Detail == 1 ? SenderNote.ActiveFromNextRestart : SenderNote.RestartNotObserved,
                Message = r.Detail == 1 ? "active from the next restart" : "restarting; validation not observed (no polling)",
            });
        }
        else if (!IsFirmware && r.Phase == ResultPhase.Applied)
        {
            if (r.Detail == 1) _eng.Info("web UI: the display serves the new file from its next restart (cache reload failed)");
            EnterWaitWebUi(now);
        }
    }

    private void HandleAbort(AbortMsg a, long now)
    {
        if (a.Reason == Reason.SessionUnknown)
        {
            if (_st == St.Applying)
            {
                // The display may already be restarting (5.6, sender side). Without the
                // RESULT, T_apply is the first APPLY: the restart may have come any time
                // since then, and 5.7 decides by "a restart since T_apply".
                if (IsFirmware && _opt.ApplyMode == ApplyMode.Now) { _tApply = _tApplyFirst; EnterWaitReboot(now); }
                else if (IsFirmware) Finish(now, UpdateOutcome.Of(OutcomeKind.Unknown, SenderNote.NoActivationConfirmation, "no confirmation of the activation"));
                else EnterWaitWebUi(now);
                return;
            }
            if (InTransfer || _st == St.WaitResult) { EnterDiagnose(now); return; }
            if (_st is St.WaitReboot or St.WaitWebUi) return;
        }
        Finish(now, new UpdateOutcome { Kind = OutcomeKind.Aborted, Reason = a.Reason, ByDevice = true });
    }

    private void HandleInfo(InfoMsg info, int src, long now)
    {
        bool restarted = Restarted(info, now);
        if (src != _dev.Address)
        {
            _eng.Info($"{TargetName}: display {_dev.Uid} now answers from address {src} (was {_dev.Address})");
            _dev.Address = src;
        }
        _dev.Info = info;
        _dev.InfoAtUs = now;
        _lastInfo = info;
        _lastInfoUs = now;
        _refUptime = info.UptimeSeconds;
        _refUs = now;
        _refTimed = true;
        _lastHeardUs = now;

        switch (_st)
        {
            case St.WaitReboot: EvaluateReboot(info, now); break;
            case St.WaitWebUi:
                if (info.WebUiId == _img.Id)
                    Finish(now, new UpdateOutcome { Kind = OutcomeKind.Success, Info = info, Message = $"web UI {info.WebUiVersion} installed" });
                else
                    Finish(now, new UpdateOutcome { Kind = OutcomeKind.Unknown, Info = info, Note = SenderNote.WebUiNotConfirmed, Message = $"the display reports web UI id {info.WebUiId}, expected {_img.Id}" });
                break;
            case St.Diagnose:
                if (restarted)
                    Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.SessionUnknown, ByDevice = true, Info = info, Note = SenderNote.DisplayRestarted, Message = "the display restarted during the transfer" });
                else
                    Finish(now, new UpdateOutcome { Kind = OutcomeKind.Aborted, Reason = Reason.SessionUnknown, ByDevice = true, Info = info, Note = SenderNote.SessionDropped, Message = "the display no longer holds the session" });
                break;
            case St.Recover when _rec == Rec.Rediscover:
                if (restarted)
                {
                    Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.SessionUnknown, ByDevice = true, Info = info, Note = SenderNote.DisplayRestarted, Message = "the display restarted during the transfer" });
                    return;
                }
                if (info.SessionState is SessionState.Idle or SessionState.Failed)
                {
                    Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.Ok, ByDevice = true, Info = info, Note = SenderNote.SessionDropped, Message = "the display dropped the session" });
                    return;
                }
                if (info.SessionId != _sid)
                {
                    // The display holds a session, but not this one: RESUME would hand
                    // this stream to someone else's session, or be refused BUSY.
                    Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.Ok, ByDevice = true, Info = info, Note = SenderNote.SessionDropped, Message = $"the display holds another session ({info.SessionId}) now" });
                    return;
                }
                _rec = Rec.ResumeOffer;
                _attempts = 0;
                _nextAction = now;
                break;
        }
    }

    /// <summary>True if INFO's uptime is clearly lower than it would be without a restart.</summary>
    private bool Restarted(InfoMsg info, long now)
    {
        // Without the time of the reference INFO only a falling uptime proves a restart.
        double expected = _refTimed ? _refUptime + (now - _refUs) / 1e6 : _refUptime;
        return info.UptimeSeconds + 10 < expected;
    }

    private void EvaluateReboot(InfoMsg info, long now)
    {
        bool slotKnown = _sOld != 0xFF && info.RunningSlot != 0xFF;
        bool restartedSinceApply = info.UptimeSeconds * 1_000_000L < now - _tApply;
        bool otherSlot = slotKnown ? info.RunningSlot != _sOld : restartedSinceApply;

        if (otherSlot && info.RunningImageId == _idNew)
        {
            if (info.OtaState == OtaState.Valid)
            {
                Finish(now, new UpdateOutcome { Kind = OutcomeKind.Success, Info = info, Message = $"firmware {info.RunningVersion} running and validated" });
                return;
            }
            Report(now, SenderPhase.WaitingForValidation, force: true,
                   verifyLeft: info.VerifySecondsLeft == 0xFFFF ? null : info.VerifySecondsLeft);
            return;
        }
        if (!otherSlot && (info.Flags & StatusFlags.RolledBack) != 0 && info.LastInvalidImageId == _idNew)
        {
            Finish(now, new UpdateOutcome
            {
                Kind = OutcomeKind.RolledBack, Cause = info.RollbackCause, Detail = info.RollbackDetail, Info = info,
                Message = $"the display rolled back to {info.RunningVersion}",
            });
            return;
        }
        if (!otherSlot && (info.Flags & StatusFlags.RestartPending) != 0)
        {
            // Also set during the 10 s countdown; only "postponed" once that is surely over
            // and no alarm holds the countdown (7.4).
            bool alarm = (info.Flags & StatusFlags.AlarmActive) != 0;
            if (!alarm && now - _tApply >= _t.PostponedAfterApplyUs && !restartedSinceApply)
            {
                Finish(now, new UpdateOutcome { Kind = OutcomeKind.InstalledRestartPostponed, Info = info, Message = "restart postponed on the display; active from the next restart" });
                return;
            }
            Report(now, SenderPhase.WaitingForValidation, force: false, message: alarm ? "restart waits for the alarm to clear" : "restart countdown");
            return;
        }
        if (restartedSinceApply)
        {
            Finish(now, new UpdateOutcome { Kind = OutcomeKind.Unknown, Info = info, Note = SenderNote.RestartedWithoutNewImage, Message = "the display restarted, but neither runs the new image nor reports a rollback" });
            return;
        }
        Report(now, SenderPhase.WaitingForValidation, force: false);
    }

    // ── States ────────────────────────────────────────────────────────────────
    private void EnterWaitResult(long now)
    {
        if (_st == St.WaitResult) return;
        _st = St.WaitResult;
        _resultDeadline = now + _t.ResultUs;
        _nextAction = now + _t.ResultQuerySilenceUs;
        _queue.Clear();
        Report(now, SenderPhase.Verifying, force: true);
    }

    private void EnterApplying(long now)
    {
        _st = St.Applying;
        _sOld = _dev.Info.RunningSlot;
        _idNew = _img.Id;
        _attempts = 0;
        SendApply(now);
        Report(now, SenderPhase.Applying, force: true);
    }

    private void SendApply(long now)
    {
        if (_attempts == 0) _tApplyFirst = now;
        _attempts++;
        _eng.SendControl(_dev.Address, new ApplyMsg { SessionId = _sid, Mode = WireMode });
        _deadline = now + _t.ApplyReplyUs;
    }

    private void EnterWaitReboot(long now)
    {
        _st = St.WaitReboot;
        _deadline = now + _t.RebootLimitUs;
        _nextAction = now + _t.RebootPollUs;
        _eng.Info("firmware: activated; polling the display until it validates the new image (do not close)");
        Report(now, SenderPhase.WaitingForValidation, force: true);
    }

    private void EnterWaitWebUi(long now)
    {
        _st = St.WaitWebUi;
        _attempts = 0;
        _nextAction = now;
        Report(now, SenderPhase.CheckingWebUi, force: true);
    }

    private void EnterDiagnose(long now)
    {
        _st = St.Diagnose;
        _attempts = 0;
        _nextAction = now;
        Report(now, SenderPhase.Recovering, force: true, message: "the display does not know the session; checking whether it restarted");
    }

    private void EnterRecover(long now, Rec start)
    {
        if (_st == St.Recover) return;
        _st = St.Recover;
        _rec = start;
        _recStart = now;
        _recDeadline = Math.Max(_lastHeardUs + _t.RecoverBudgetUs, now + 5_000_000);
        _recRounds = 0;
        _attempts = 0;
        _nextAction = now;
        _eng.Info($"{TargetName}: recovering ({start})");
        Report(now, SenderPhase.Recovering, force: true);
    }

    // ── Tick ──────────────────────────────────────────────────────────────────
    public long Tick(long now)
    {
        if (_st == St.Done) return long.MaxValue;
        if (_eng.LinkLost && _st is not (St.Recover or St.WaitReboot or St.Offering or St.Done))
            EnterRecover(now, Rec.Reopen);

        long next = _st switch
        {
            St.Offering   => TickOffering(now),
            St.Confirming => TickConfirming(now),
            St.WaitCredit => TickWaitCredit(now),
            St.Sending    => TickSending(now),
            St.WaitAck    => TickWaitAck(now),
            St.WaitResult => TickWaitResult(now),
            St.Applying   => TickApplying(now),
            St.WaitReboot => TickWaitReboot(now),
            St.WaitWebUi  => TickInfoQuery(now, "the display did not confirm the new web UI"),
            St.Diagnose   => TickInfoQuery(now, "the display no longer holds the session"),
            St.Recover    => TickRecover(now),
            _             => long.MaxValue,
        };
        if (_st != St.Done && now - _lastProgressUs >= 500_000)
            Report(now, CurrentPhase, force: false);
        return Math.Min(next, _st == St.Done ? long.MaxValue : now + 500_000);
    }

    private long TickOffering(long now)
    {
        if (_eng.LinkLost) { EnterRecover(now, Rec.Reopen); return now; }
        if (now < _deadline) return _deadline;
        if (_attempts < _t.OfferAttempts) { SendOffer(now); return _deadline; }
        Finish(now, UpdateOutcome.Of(OutcomeKind.Failed, SenderNote.NoOfferReply, "no answer to the OFFER"));
        return long.MaxValue;
    }

    private long TickConfirming(long now)
    {
        if (now >= _deadline)
        {
            // The display rejects with CONFIRM_TIMEOUT after 60 s; that reply was lost.
            SendAbort(Reason.Timeout);
            Finish(now, new UpdateOutcome { Kind = OutcomeKind.Rejected, Reason = Reason.ConfirmTimeout, Note = SenderNote.NoDecision, Message = "no decision from the display" });
            return long.MaxValue;
        }
        if (now >= _nextAction)
        {
            _eng.SendControl(_dev.Address, Query(QueryWhat.Status));
            _nextAction = now + _t.ConfirmKeepaliveUs;
        }
        return Math.Min(_deadline, _nextAction);
    }

    private long TickWaitCredit(long now)
    {
        if (now < _deadline) return _deadline;
        return QueryAck(now);
    }

    private long TickWaitAck(long now)
    {
        if (now < _deadline) return _deadline;
        return QueryAck(now);
    }

    /// <summary>QUERY(ACK) every second, at most five times, then recovery (5.4 step 4).</summary>
    private long QueryAck(long now)
    {
        if (_ackQueries >= _t.AckQueryAttempts)
        {
            EnterRecover(now, Rec.Rediscover);
            return now;
        }
        _ackQueries++;
        _eng.SendControl(_dev.Address, Query(QueryWhat.Ack));
        _deadline = now + _t.AckQueryIntervalUs;
        return _deadline;
    }

    private long TickSending(long now)
    {
        // One transfer per bus (6.5): pause while another source's DATA is around.
        if (_eng.LastForeignDataUs != long.MinValue && now - _eng.LastForeignDataUs < _t.ForeignQuietUs)
        {
            if (!_paused)
            {
                _paused = true;
                _pausedSince = now;
                _nextAction = now + _t.PauseKeepaliveUs;
                _eng.Info($"{TargetName}: another transfer on the bus, pausing");
                Report(now, SenderPhase.PausedForeignTransfer, force: true);
            }
            if (now - _pausedSince > _t.ForeignPauseMaxUs)
            {
                SendAbort(Reason.SenderCancelled);
                Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.Busy, Note = SenderNote.BusBusy, Message = "another transfer kept the bus busy for 60 s" });
                return long.MaxValue;
            }
            if (now >= _nextAction)
            {
                // Keep the display's 60 s idle timer from running out.
                _eng.SendControl(_dev.Address, Query(QueryWhat.Ack));
                _nextAction = now + _t.PauseKeepaliveUs;
            }
            return Math.Min(_nextAction, _eng.LastForeignDataUs + _t.ForeignQuietUs);
        }
        if (_paused)
        {
            _paused = false;
            _eng.Info($"{TargetName}: the bus is free again, continuing");
        }

        while (_queue.Count > 0)
        {
            int k = _queue.Peek();
            if (_sends[k] >= _t.MaxSendsPerChunk)
            {
                SendAbort(Reason.LinkUnreliable);
                Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.LinkUnreliable, Note = SenderNote.ChunkRepeated, Message = $"chunk {k} sent {_sends[k]} times" });
                return long.MaxValue;
            }
            if (_pendingChunk != k)
            {
                int off = k * Chunk;
                int len = Math.Min(Chunk, _img.Stream.Length - off);
                _pendingPayload = DataMsg.Build(_sid, (uint)off, _img.Stream.AsSpan(off, len));
                _pendingChunk = k;
            }
            if (!_eng.TrySendData(_dev.Address, _pendingPayload!, out long retry))
                return retry;
            if (_sends[k] > 0) Resends++;
            _sends[k]++;
            _queue.Dequeue();
            _pendingChunk = -1;
        }
        _st = St.WaitAck;
        _ackQueries = 0;
        _deadline = now + _t.AckAfterQueueUs;
        return _deadline;
    }

    private long TickWaitResult(long now)
    {
        if (now >= _resultDeadline)
        {
            SendAbort(Reason.Timeout);
            Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.Timeout, Note = SenderNote.NoResult, Message = "no RESULT from the display" });
            return long.MaxValue;
        }
        // QUERY(RESULT) after 5 s without any message (5.5); every message pushes it out.
        if (now >= _nextAction)
        {
            _eng.SendControl(_dev.Address, Query(QueryWhat.Result));
            _nextAction = now + _t.ResultQuerySilenceUs;
        }
        return Math.Min(_resultDeadline, _nextAction);
    }

    private long TickApplying(long now)
    {
        if (_askResult)
        {
            // The display reported DONE but its RESULT got lost: ask for it.
            _askResult = false;
            _eng.SendControl(_dev.Address, Query(QueryWhat.Result));
        }
        if (now < _deadline) return _deadline;
        if (_attempts < _t.ApplyAttempts) { SendApply(now); return _deadline; }
        // No answer to three attempts: the display may already be restarting (5.6).
        // T_apply is then the first APPLY: a restart since then counts (5.7).
        if (!IsFirmware) { EnterWaitWebUi(now); return now; }
        if (_opt.ApplyMode == ApplyMode.Now) { _tApply = _tApplyFirst; EnterWaitReboot(now); return now; }
        Finish(now, UpdateOutcome.Of(OutcomeKind.Unknown, SenderNote.NoActivationConfirmation, "no confirmation of the activation"));
        return long.MaxValue;
    }

    private long TickWaitReboot(long now)
    {
        if (!_eng.Link.IsOpen || _eng.LinkLost)
        {
            // The polls are the new firmware's reception proof (H5): get the adapter back.
            if (now >= _nextAction)
            {
                _eng.TryReopenLink();
                _nextAction = now + _t.ReopenIntervalUs;
            }
        }
        else if (now >= _nextAction)
        {
            _eng.SendControl(UpdProto.Broadcast, new QueryMsg { SessionId = 0, What = QueryWhat.Info, UidFilter = _dev.Uid });
            _nextAction = now + _t.RebootPollUs;
        }
        if (now >= _deadline)
        {
            // "Postponed" only if the display still answers with a pending restart (an alarm
            // held the countdown); a display that went quiet is unknown (5.7, last row).
            bool stillPending = _lastInfo is { } li && now - _lastInfoUs < 30_000_000 &&
                                (li.Flags & StatusFlags.RestartPending) != 0 && li.RunningSlot == _sOld;
            bool noInfo = _lastInfoUs < _tApply || now - _lastInfoUs > 30_000_000;
            Finish(now, stillPending
                ? new UpdateOutcome { Kind = OutcomeKind.InstalledRestartPostponed, Info = _lastInfo, Message = "restart pending on the display" }
                : new UpdateOutcome
                {
                    Kind = OutcomeKind.Unknown, Info = _lastInfo,
                    Note = noInfo ? SenderNote.NoInfo : SenderNote.NoDecisionInTime,
                    Message = _lastInfoUs < _tApply ? "no INFO for 5 minutes (display off, or a 4-inch in download mode)"
                            : now - _lastInfoUs > 30_000_000 ? $"no INFO for {(now - _lastInfoUs) / 1_000_000} s (display off, or a 4-inch in download mode)"
                            : "no decision within 5 minutes",
                });
            return long.MaxValue;
        }
        return Math.Min(_deadline, _nextAction);
    }

    private long TickInfoQuery(long now, string giveUp)
    {
        if (now < _nextAction) return _nextAction;
        if (_attempts >= _t.InfoQueryAttempts)
        {
            Finish(now, new UpdateOutcome
            {
                Kind = _st == St.Diagnose ? OutcomeKind.Aborted : OutcomeKind.Unknown,
                Reason = _st == St.Diagnose ? Reason.SessionUnknown : Reason.Ok,
                ByDevice = _st == St.Diagnose,
                Note = _st == St.Diagnose ? SenderNote.SessionDropped : SenderNote.WebUiNotConfirmed,
                Message = giveUp,
            });
            return long.MaxValue;
        }
        _attempts++;
        _eng.SendControl(UpdProto.Broadcast, new QueryMsg { SessionId = 0, What = QueryWhat.Info, UidFilter = _dev.Uid });
        _nextAction = now + _t.InfoQueryIntervalUs;
        return _nextAction;
    }

    private long TickRecover(long now)
    {
        if (now >= _recDeadline)
        {
            Finish(now, _recoverReject == Reason.Busy
                ? new UpdateOutcome { Kind = OutcomeKind.Rejected, Reason = Reason.Busy, ByDevice = true, Message = "the display kept the session busy for another sender" }
                : new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.Timeout, Note = SenderNote.DisplayUnreachable, Message = "the display could not be reached again within 55 s" });
            return long.MaxValue;
        }
        // The adapter may go away again while we rediscover or resume.
        if ((!_eng.Link.IsOpen || _eng.LinkLost) && _rec != Rec.Reopen)
        {
            _rec = Rec.Reopen;
            _nextAction = now;
        }
        if (now < _nextAction) return Math.Min(_nextAction, _recDeadline);
        switch (_rec)
        {
            case Rec.Reopen:
                if (_eng.Link.IsOpen && !_eng.LinkLost) { _rec = Rec.Rediscover; _attempts = 0; return now; }
                if (now - _recStart > _t.ReopenMaxUs)
                {
                    Finish(now, UpdateOutcome.Of(OutcomeKind.Failed, SenderNote.AdapterGone, "the CAN adapter did not come back within 30 s"));
                    return long.MaxValue;
                }
                if (_eng.TryReopenLink()) { _rec = Rec.Rediscover; _attempts = 0; _nextAction = now + 300_000; }
                else _nextAction = now + _t.ReopenIntervalUs;
                break;

            case Rec.Rediscover:
                // Ask by unique id until the display answers (HandleInfo decides) or the
                // budget runs out. Never RESUME blind: a display that does not answer may
                // be starting up, and would refuse it NOT_READY or - holding no session
                // any more - take it for a new OFFER with a new dialog.
                _attempts++;
                _eng.SendControl(UpdProto.Broadcast, new QueryMsg { SessionId = 0, What = QueryWhat.Info, UidFilter = _dev.Uid });
                _nextAction = now + _t.InfoQueryIntervalUs;
                break;

            case Rec.ResumeOffer:
                if ((_dev.Info.Caps & Capabilities.Resume) == 0)
                {
                    Finish(now, UpdateOutcome.Of(OutcomeKind.Failed, SenderNote.DisplayUnreachable, "lost the display, and it cannot resume"));
                    return long.MaxValue;
                }
                if (_attempts >= _t.OfferAttempts)
                {
                    if (++_recRounds >= _t.RecoverRounds)
                    {
                        Finish(now, new UpdateOutcome { Kind = OutcomeKind.Failed, Reason = Reason.Timeout, Note = SenderNote.NoResumeReply, Message = "no answer to OFFER RESUME" });
                        return long.MaxValue;
                    }
                    _rec = Rec.Rediscover;
                    _attempts = 0;
                    return now;
                }
                _resume = true;
                _attempts++;
                _offerSent = true;
                _eng.SendControl(_dev.Address, BuildOffer(resume: true));
                _nextAction = now + _t.OfferReplyUs;
                break;
        }
        return Math.Min(_nextAction, _recDeadline);
    }

    // ── Events from the engine ────────────────────────────────────────────────
    public void OnLinkEvent(LinkEventKind kind, long now)
    {
        if (kind == LinkEventKind.Bell || _st is St.Done or St.WaitReboot) return;
        EnterRecover(now, Rec.Reopen);
    }

    public void OnAddressChanged(long now)
    {
        if (_st is St.Done or St.WaitReboot or St.WaitWebUi or St.Diagnose) return;
        if (_st == St.Offering) { _attempts = 0; SendOffer(now); return; }
        // Messages from our old address are ignored now: continue with RESUME (5.8).
        EnterRecover(now, Rec.ResumeOffer);
    }

    /// <summary>Cancellation by the caller (5.10, last paragraph).</summary>
    public void Cancel(long now)
    {
        if (_st == St.Done) return;
        if (_st == St.WaitReboot)
        {
            Finish(now, new UpdateOutcome
            {
                Kind = OutcomeKind.Unknown, Info = _lastInfo, Note = SenderNote.PollingStopped,
                Message = "polling stopped before the display validated the new firmware: it will roll back (H5)",
            });
            return;
        }
        if (_st == St.WaitWebUi) { Finish(now, UpdateOutcome.Of(OutcomeKind.Unknown, SenderNote.WebUiNotConfirmed, "cancelled while checking the web UI")); return; }
        if (_offerSent) SendAbort(Reason.SenderCancelled);
        Finish(now, new UpdateOutcome { Kind = OutcomeKind.Aborted, Reason = Reason.SenderCancelled, ByDevice = false, Note = SenderNote.Cancelled });
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    private QueryMsg Query(QueryWhat what) => new() { SessionId = _sid, What = what, UidFilter = _dev.Uid };

    private void SendAbort(Reason r) => _eng.SendControl(_dev.Address, new AbortMsg { SessionId = _sid, Reason = r });

    private void Finish(long now, UpdateOutcome o)
    {
        if (_st == St.Done) return;
        _st = St.Done;
        // Over, whatever the outcome: nothing to RESUME any more, and stop-and-wait
        // (if the OFFER probe chose it) was for this session only (6.4).
        if (_remembered) _eng.Sessions.Forget(_opt.CacheDirectory, _dev.Uid, _sid);
        _eng.SessionEnded();
        Outcome = o.Info is null ? o with { Info = _lastInfo } : o;
        _eng.Info($"{TargetName}: {Outcome}" + (Resends > 0 ? $" ({Resends} chunks sent again)" : ""));
        Report(now, SenderPhase.Done, force: true, message: Outcome.ToString());
    }

    private SenderPhase CurrentPhase => _st switch
    {
        St.Offering   => SenderPhase.Offering,
        St.Confirming => SenderPhase.WaitingForConfirmation,
        St.WaitCredit => _highestBase == 0 && !_holdMode ? SenderPhase.Preparing : SenderPhase.Transferring,
        St.Sending    => _paused ? SenderPhase.PausedForeignTransfer : SenderPhase.Transferring,
        St.WaitAck    => SenderPhase.Transferring,
        St.WaitResult => SenderPhase.Verifying,
        St.Applying   => SenderPhase.Applying,
        St.WaitReboot => SenderPhase.WaitingForValidation,
        St.WaitWebUi  => SenderPhase.CheckingWebUi,
        St.Recover or St.Diagnose => SenderPhase.Recovering,
        _             => SenderPhase.Done,
    };

    private void Report(long now, SenderPhase phase, bool force, string? message = null, int? verifyLeft = null)
    {
        if (_progress is null) return;
        if (!force && now - _lastProgressUs < 250_000) return;
        _lastProgressUs = now;
        var last = _eng.Limiter.Last;
        // The dialog countdown runs on between the display's reports (every 5 s).
        int? dialog = null;
        if (phase == SenderPhase.WaitingForConfirmation && _dialogEndUs >= 0)
            dialog = (int)Math.Max(0, (_dialogEndUs - now + 999_999) / 1_000_000);
        _progress(new UpdateProgress
        {
            Target = _img.Target,
            Phase = phase,
            StreamAccepted = _highestBase,
            StreamSize = _img.StreamSize,
            EtaSeconds = Eta(now),
            DialogSecondsLeft = dialog,
            VerifySecondsLeft = verifyLeft ?? (_lastInfo is { VerifySecondsLeft: not 0xFFFF } li && _st == St.WaitReboot ? li.VerifySecondsLeft : null),
            OwnLoad = last.OwnLoad,
            TotalLoad = last.TotalLoad,
            EffectiveShare = _eng.Limiter.EffectiveShare,
            // From the APPLY (mode 0) on: the display may be counting down already, and
            // without this sender's polls after the restart it rolls back (H5, 12.7).
            MustStayConnected = (_st == St.WaitReboot || (_st == St.Applying && IsFirmware)) && _opt.ApplyMode == ApplyMode.Now,
            Message = message ?? "",
        });
    }

    /// <summary>Remaining transfer time from the measured stream rate over the last ~20 s.</summary>
    private double? Eta(long now)
    {
        if (_rate.Count < 2 || _highestBase >= _img.StreamSize) return null;
        var last = _rate[^1];
        var first = _rate.FirstOrDefault(r => last.Us - r.Us <= 20_000_000);
        if (first.Us == last.Us) first = _rate[^2];
        double dt = (last.Us - first.Us) / 1e6;
        if (dt <= 0) return null;
        double rate = (last.Base - first.Base) / dt;
        if (rate <= 0) return null;
        while (_rate.Count > 200) _rate.RemoveAt(0);
        return (_img.StreamSize - _highestBase) / rate;
    }
}
