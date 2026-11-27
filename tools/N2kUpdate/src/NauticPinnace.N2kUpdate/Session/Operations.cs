using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Session;

/// <summary>
/// Between updates nothing else reopens an adapter that went away (5.8 covers a
/// session): discovery and identify reopen it themselves, once a second, and give
/// up after <see cref="LimitUs"/>.
/// </summary>
internal sealed class LinkKeeper
{
    public const long LimitUs = 5_000_000;
    private long _lostSince = long.MinValue, _next = long.MinValue;

    /// <summary>Null: the link is usable. Otherwise the time to look again, or
    /// <see cref="long.MaxValue"/> once it is given up.</summary>
    public long? Check(N2kEngine eng, long now)
    {
        if (!eng.LinkLost && eng.Link.IsOpen) { _lostSince = long.MinValue; return null; }
        if (_lostSince == long.MinValue) _lostSince = now;
        if (now >= _next)
        {
            _next = now + 1_000_000;
            if (eng.TryReopenLink()) { _lostSince = long.MinValue; return null; }
        }
        return now - _lostSince >= LimitUs ? long.MaxValue : _next;
    }
}

/// <summary>
/// Discovery (5.2): QUERY(INFO) to 255, three rounds one second apart, plus an ISO
/// request for PGN 126996 so that NauticPinnace displays without a receiver can be
/// listed with a hint. Displays are keyed by unique id, never by address.
/// </summary>
public sealed class DiscoveryOperation : EngineOperation
{
    private readonly N2kEngine _eng;
    private readonly SessionTimers _t;
    private readonly bool _askProductInfo;
    private readonly Dictionary<UniqueId, DisplayDevice> _found = new();
    private readonly Dictionary<int, ProductInfo> _products = new();
    private readonly LinkKeeper _link = new();
    private int _round;
    private long _next = long.MinValue;

    public DiscoveryOperation(N2kEngine engine, SessionTimers? timers = null, bool askProductInfo = true)
    {
        _eng = engine;
        _t = timers ?? new SessionTimers();
        _askProductInfo = askProductInfo;
    }

    public DiscoveryResult? Result { get; private set; }
    public event Action<DiscoveryResult>? Completed;

    public override void OnMessage(UpdateMessage msg, int source, int destination, long nowUs)
    {
        if (msg is not InfoMsg info || msg.ProtocolVersion != UpdProto.Version) return;
        if (_found.TryGetValue(info.Uid, out var d))
        {
            d.Address = source;
            d.Info = info;
            d.InfoAtUs = nowUs;
        }
        else
        {
            _found[info.Uid] = new DisplayDevice(source, info, nowUs);
        }
    }

    public override void OnProductInfo(int source, ProductInfo info, long nowUs) => _products[source] = info;

    public override long Tick(long nowUs)
    {
        if (IsFinished) return long.MaxValue;
        if (_link.Check(_eng, nowUs) is long retry)
        {
            if (retry != long.MaxValue) return retry;
            Result = new DiscoveryResult(_found.Values.ToList(), [], AdapterLost: true);
            IsFinished = true;
            _eng.Info("discovery: the CAN adapter is gone and could not be reopened");
            Completed?.Invoke(Result);
            return long.MaxValue;
        }
        if (!_eng.Source.IsUsable) return nowUs + 50_000;
        if (_next == long.MinValue) _next = nowUs;
        if (nowUs < _next) return _next;
        if (_round < _t.DiscoverRounds)
        {
            _eng.SendControl(UpdProto.Broadcast, new QueryMsg { SessionId = 0, What = QueryWhat.Info });
            if (_round == 0 && _askProductInfo)
            {
                // Product information finds displays without a receiver; the address
                // claims refresh the list of addresses in use.
                _eng.SendIsoRequest(N2kPgn.ProductInfo, UpdProto.Broadcast);
                _eng.SendIsoRequest(N2kPgn.IsoAddressClaim, UpdProto.Broadcast);
            }
            _round++;
            _next = nowUs + _t.DiscoverRoundUs;
            return _next;
        }
        var displays = _found.Values.OrderBy(d => d.Address).ToList();
        var answered = displays.Select(d => d.Address).ToHashSet();
        var legacy = _products
            .Where(p => p.Value.ModelId.Equals("NauticPinnace", StringComparison.OrdinalIgnoreCase) && !answered.Contains(p.Key))
            .Select(p => new LegacyDisplay(p.Key, p.Value.SoftwareVersion))
            .OrderBy(l => l.Address)
            .ToList();
        Result = new DiscoveryResult(displays, legacy);
        IsFinished = true;
        _eng.Info($"discovery: {displays.Count} display(s) with update support" + (legacy.Count > 0 ? $", {legacy.Count} without" : ""));
        Completed?.Invoke(Result);
        return long.MaxValue;
    }

    public override void Cancel(long nowUs)
    {
        if (IsFinished) return;
        Result = new DiscoveryResult(_found.Values.ToList(), []);
        IsFinished = true;
        Completed?.Invoke(Result);
    }

    public override void OnCrash(Exception ex, long nowUs) => Cancel(nowUs);
}

/// <summary>IDENTIFY (3.11): one message, no reply.</summary>
public sealed class IdentifyOperation : EngineOperation
{
    private readonly N2kEngine _eng;
    private readonly DisplayDevice _dev;
    private readonly byte _seconds;
    private readonly LinkKeeper _link = new();

    public IdentifyOperation(N2kEngine engine, DisplayDevice device, int seconds)
    {
        _eng = engine;
        _dev = device;
        _seconds = (byte)Math.Clamp(seconds, 1, 30);
    }

    /// <summary>True if IDENTIFY went out; false if the adapter could not be reopened.</summary>
    public event Action<bool>? Completed;

    public override void OnCrash(Exception ex, long nowUs)
    {
        IsFinished = true;
        Completed?.Invoke(false);
    }

    public override long Tick(long nowUs)
    {
        if (IsFinished) return long.MaxValue;
        if (_link.Check(_eng, nowUs) is long retry)
        {
            if (retry != long.MaxValue) return retry;
            IsFinished = true;
            Completed?.Invoke(false);
            return long.MaxValue;
        }
        if (!_eng.Source.IsUsable) return nowUs + 50_000;
        _eng.SendControl(_dev.Address, new IdentifyMsg { SessionId = 0, Seconds = _seconds, TargetUid = _dev.Uid });
        IsFinished = true;
        Completed?.Invoke(true);
        return long.MaxValue;
    }
}

/// <summary>
/// Everything for one display (12.2 UpdateAsync): the firmware, then the web UI of
/// the same release, either approved together with the firmware (5.3.6) or offered
/// with its own dialog (5.7, last paragraph). Several displays are updated one after
/// another, never in parallel (6.5).
/// </summary>
public sealed class UpdateJob : EngineOperation
{
    private readonly N2kEngine _eng;
    private readonly DisplayDevice _dev;
    private readonly UpdateRequest _req;
    private readonly UpdateOptions _opt;
    private readonly Action<UpdateProgress>? _progress;
    private readonly TransferImage? _fw;
    private readonly TransferImage? _web;
    private UpdateSession? _session;
    private UpdateOutcome? _fwOutcome, _webOutcome;
    private bool _started, _webStarted, _cancelled;

    /// <summary>
    /// Prepares the images (hashing, deflate) at construction, so that construction
    /// may run on any thread and the engine thread never compresses.
    /// </summary>
    public UpdateJob(N2kEngine engine, DisplayDevice device, UpdateRequest request, UpdateOptions options,
                     Action<UpdateProgress>? progress = null)
    {
        _eng = engine;
        _dev = device;
        _req = request;
        _opt = options;
        _progress = progress;
        var info = device.Info;

        if (options.InstallFirmware && request.Firmware is { } fw)
        {
            var problems = fw.CheckAgainst(info);
            if (problems.Count > 0)
                _fwOutcome = UpdateOutcome.Of(OutcomeKind.Refused, RefusalNote(fw, info), string.Join("; ", problems));
            else
            {
                bool deflate = options.Compression == CompressionMode.Auto && (info.Caps & Capabilities.Deflate) != 0;
                _fw = TransferImage.ForFirmware(fw, deflate, new DeflateCache(options.CacheDirectory));
                foreach (var w in fw.WarningsAgainst(info)) Warnings.Add(w);
            }
        }

        if (options.InstallWebUi && request.WebUi is { } web)
        {
            string? version = request.WebUiVersion ?? request.Firmware?.Descriptor?.FwVersion;
            var problems = new List<string>(web.CheckAgainst(info));
            var note = problems.Count > 0 ? RefusalNote(web, info) : SenderNote.None;
            if (!TextField.IsValidVersion(version))
            {
                problems.Add("the firmware version this web UI belongs to is unknown; enter it, or allow a version mismatch");
                if (note == SenderNote.None) note = SenderNote.WebUiVersionUnknown;
            }
            if (problems.Count > 0)
                _webOutcome = UpdateOutcome.Of(OutcomeKind.Refused, note, string.Join("; ", problems));
            else
                _web = TransferImage.ForWebUi(web, version!);
        }
        if (_fw is null && _fwOutcome is null && _web is null && _webOutcome is null)
            _fwOutcome = UpdateOutcome.Of(OutcomeKind.Refused, SenderNote.NothingToInstall, "nothing to install");
    }

    // The first of the 8.2 checks that failed, as a note a UI can put into words.
    private static SenderNote RefusalNote(FirmwareImage fw, InfoMsg info)
    {
        if (fw.IsLegacy) return SenderNote.LegacyImage;
        if (!fw.IsValid) return SenderNote.BadImage;
        if (fw.Board != info.Board) return SenderNote.WrongBoard;
        if (info.MaxFirmwareSize != 0 && (uint)fw.Size > info.MaxFirmwareSize) return SenderNote.TooLarge;
        if ((info.Caps & Capabilities.Firmware) == 0) return SenderNote.NotSupported;
        return SenderNote.IncompatibleProtocol;
    }

    private static SenderNote RefusalNote(WebUiImage web, InfoMsg info)
    {
        if (!web.IsValid) return SenderNote.BadImage;
        if (info.MaxWebUiSize != 0 && (uint)web.Size > info.MaxWebUiSize) return SenderNote.TooLarge;
        if ((info.Caps & Capabilities.WebUi) == 0) return SenderNote.NotSupported;
        return SenderNote.IncompatibleProtocol;
    }

    public DisplayDevice Device => _dev;
    public List<string> Warnings { get; } = new();
    public UpdateReport? Report { get; private set; }
    public UpdateSession? CurrentSession => _session;
    public event Action<UpdateReport>? Completed;

    public override void OnMessage(UpdateMessage msg, int source, int destination, long nowUs) =>
        _session?.OnMessage(msg, source, nowUs);

    public override void OnLinkEvent(LinkEventKind kind, long nowUs) => _session?.OnLinkEvent(kind, nowUs);

    public override void OnAddressChanged(int oldAddress, int newAddress, long nowUs) => _session?.OnAddressChanged(nowUs);

    public override void Cancel(long nowUs)
    {
        _cancelled = true;
        _session?.Cancel(nowUs);
    }

    public override void OnCrash(Exception ex, long nowUs)
    {
        var failed = UpdateOutcome.Of(OutcomeKind.Failed, "internal error: " + ex.Message);
        if (_session?.Target == UpdateTarget.WebUi) _webOutcome ??= failed;
        else _fwOutcome ??= failed;
        Report = new UpdateReport(_dev, _fwOutcome, _webOutcome);
        IsFinished = true;
        Completed?.Invoke(Report);
    }

    public override long Tick(long nowUs)
    {
        if (IsFinished) return long.MaxValue;
        if (!_started && _cancelled)
        {
            // Cancelled before anything went out (UpdateAsync posts the cancel right
            // after Add when it came while the images were prepared): no OFFER at all.
            _started = true;
            if (_fw is not null)
                _fwOutcome ??= new UpdateOutcome { Kind = OutcomeKind.Aborted, Reason = Reason.SenderCancelled, Note = SenderNote.Cancelled, Message = "cancelled before the OFFER" };
            else if (_web is not null)
                _webOutcome ??= new UpdateOutcome { Kind = OutcomeKind.Aborted, Reason = Reason.SenderCancelled, Note = SenderNote.Cancelled, Message = "cancelled before the OFFER" };
        }
        if (!_eng.Source.IsUsable) return nowUs + 50_000;

        if (!_started)
        {
            _started = true;
            if (_fw is not null) StartFirmware(nowUs);
        }

        if (_session is not null)
        {
            long t = _session.Tick(nowUs);
            if (!_session.IsFinished) return t;
            if (_session.Target == UpdateTarget.Firmware && _fwOutcome is null)
            {
                _fwOutcome = _session.Outcome;
                _dev.Info = _session.LatestInfo;
            }
            else if (_session.Target == UpdateTarget.WebUi && _webOutcome is null)
                _webOutcome = _session.Outcome;
        }

        if (!_webStarted && _web is not null && _webOutcome is null)
        {
            _webStarted = true;
            bool fwRequested = _opt.InstallFirmware && _req.Firmware is not null;
            if (_cancelled)
                _webOutcome = UpdateOutcome.Of(OutcomeKind.Skipped, SenderNote.Cancelled, "cancelled");
            else if (fwRequested && _fwOutcome is { Kind: OutcomeKind.InstalledRestartPostponed or OutcomeKind.InstalledNotConfirmed })
                // Installed, but not running yet: the web UI of the new release would not
                // match the firmware the display runs now (VERSION_MISMATCH), and its
                // approval only becomes usable once the new firmware is valid (5.3.6).
                _webOutcome = UpdateOutcome.Of(OutcomeKind.Skipped, SenderNote.FirmwareActiveAfterRestart,
                    "the firmware becomes active at the next restart; install the web UI after that");
            else if (fwRequested && _fwOutcome is not { Kind: OutcomeKind.Success })
                _webOutcome = UpdateOutcome.Of(OutcomeKind.Skipped, SenderNote.FirmwareNotInstalled, "the firmware was not installed");
            else if (fwRequested && _dev.Info.WebUiId == _web.Id)
                _webOutcome = UpdateOutcome.Of(OutcomeKind.Skipped, SenderNote.WebUiAlreadyInstalled, "this web UI is already installed");
            else if (!fwRequested && !_opt.AllowWebUiVersionMismatch && _web.Version != _dev.Info.RunningVersion)
                _webOutcome = UpdateOutcome.Of(OutcomeKind.Refused, SenderNote.WebUiVersionMismatch,
                    $"the web UI belongs to firmware {_web.Version}, the display runs {_dev.Info.RunningVersion} (allow a version mismatch to install it anyway)");
            else
            {
                StartWebUi(nowUs);
                return nowUs;
            }
        }

        Report = new UpdateReport(_dev, _fwOutcome, _webOutcome);
        IsFinished = true;
        Completed?.Invoke(Report);
        return long.MaxValue;
    }

    /// <summary>
    /// RESUME (5.3.4) only for a session this sender can show is its own: the display
    /// holds a session (INFO, CONFIRMING..DONE), and its id is one this sender opened for
    /// exactly this stream (<see cref="SessionMemory"/>). Then the session id is reused, so
    /// the OFFER is the same session again. Otherwise a second sender with the same release
    /// would take over a running transfer (5.8); a sender that cannot show it offers anew.
    /// </summary>
    private bool ResumePossible(TransferImage img) =>
        (_dev.Info.Caps & Capabilities.Resume) != 0 &&
        _dev.Info.SessionState is >= SessionState.Confirming and <= SessionState.Done &&
        _eng.Sessions.IsOwn(_opt.CacheDirectory, _dev.Uid, _dev.Info.SessionId, img);

    private (byte Sid, bool Resume) SessionFor(TransferImage img) =>
        ResumePossible(img) ? (_dev.Info.SessionId, true) : (_eng.NextSessionId(), false);

    private void StartFirmware(long now)
    {
        var info = _dev.Info;
        WebUiImage? companion = null;
        if (_opt.IncludeCompanionWebUi && _web is not null && _req.WebUi is { } w &&
            (info.Caps & Capabilities.CompanionWebUi) != 0 && info.WebUiId != _web.Id)
            companion = w;
        var (sid, resume) = SessionFor(_fw!);
        _session = new UpdateSession(_eng, _dev, _fw!, _opt, companion, sid, resume, _progress);
        _session.Begin(now);
    }

    private void StartWebUi(long now)
    {
        // A firmware session that just finished holds no session on the display any more.
        var (sid, resume) = _fw is null ? SessionFor(_web!) : (_eng.NextSessionId(), false);
        if ((_dev.Info.Flags & StatusFlags.CompanionApproved) != 0)
            _eng.Info("web UI: approved together with the firmware, no second confirmation needed");
        _session = new UpdateSession(_eng, _dev, _web!, _opt, null, sid, resume, _progress);
        _session.Begin(now);
    }
}
