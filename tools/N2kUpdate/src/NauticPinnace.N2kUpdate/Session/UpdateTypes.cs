using NauticPinnace.N2kUpdate.Bus;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Session;

/// <summary>A display found by discovery. Identified by unique id; the address may change (5.2).</summary>
public sealed class DisplayDevice
{
    public DisplayDevice(int address, InfoMsg info, long infoAtUs = 0)
    {
        Address = address;
        Info = info;
        InfoAtUs = infoAtUs;
    }

    public int Address { get; set; }
    public InfoMsg Info { get; set; }

    /// <summary>Engine time at which <see cref="Info"/> was received (for restart detection by uptime).</summary>
    public long InfoAtUs { get; set; }
    public UniqueId Uid => Info.Uid;
    public BoardId Board => Info.Board;

    public override string ToString() =>
        $"{Boards.DisplayName(Info.Board)} \"{Info.Label}\" {Info.RunningVersion} @ {Address} ({Uid})";
}

/// <summary>A NauticPinnace that answered PGN 126996 but not QUERY(INFO): no receiver yet (5.2).</summary>
public sealed record LegacyDisplay(int Address, string SoftwareVersion);

/// <summary>What discovery found (5.2).</summary>
/// <param name="Displays">Displays that answered QUERY(INFO), by unique id.</param>
/// <param name="Legacy">NauticPinnace displays without a receiver (PGN 126996 only).</param>
/// <param name="AdapterLost">The CAN adapter went away and could not be reopened: the lists are incomplete.</param>
public sealed record DiscoveryResult(IReadOnlyList<DisplayDevice> Displays, IReadOnlyList<LegacyDisplay> Legacy,
                                     bool AdapterLost = false);

/// <summary>How the firmware is activated after the transfer (12.5).</summary>
public enum ApplyMode
{
    /// <summary>APPLY mode 0: restart after the countdown; the sender polls until the image is validated.</summary>
    Now,
    /// <summary>APPLY mode 2: like Now, but the sender does not poll (H5 then accepts any N2K frame).</summary>
    NowNoWait,
    /// <summary>APPLY mode 1: activate, active from the next restart.</summary>
    NextRestart,
    /// <summary>Transfer and verify only, no APPLY.</summary>
    None,
}

public enum CompressionMode { Auto, Off }

/// <summary>Sender timers (5.11). Fixed in v1; tests may shorten them.</summary>
public sealed record SessionTimers
{
    public long DiscoverRoundUs { get; init; } = 1_000_000;
    public int DiscoverRounds { get; init; } = 3;
    public long OfferReplyUs { get; init; } = 2_000_000;
    public int OfferAttempts { get; init; } = 3;
    public long ConfirmKeepaliveUs { get; init; } = 5_000_000;
    public long ConfirmExtraUs { get; init; } = 15_000_000;
    public long FirstCreditUs { get; init; } = 10_000_000;
    public long AckAfterQueueUs { get; init; } = 300_000;
    public long AckQueryIntervalUs { get; init; } = 1_000_000;
    public int AckQueryAttempts { get; init; } = 5;
    public long HoldExtraUs { get; init; } = 3_000_000;
    /// <summary>Wait for a HOLD of 0xFFFF ("unknown"); STATUS every second extends it.</summary>
    public long HoldUnknownUs { get; init; } = 10_000_000;
    public long ResultUs { get; init; } = 120_000_000;
    public long ResultQuerySilenceUs { get; init; } = 5_000_000;
    public long ApplyReplyUs { get; init; } = 10_000_000;
    public int ApplyAttempts { get; init; } = 3;
    public long RebootPollUs { get; init; } = 3_000_000;
    public long RebootLimitUs { get; init; } = 300_000_000;
    /// <summary>
    /// INFO shows "restart pending" during the 10 s countdown too (4.5), so it only
    /// counts as "postponed" this long after APPLY. See the note on 5.7 in the README.
    /// </summary>
    public long PostponedAfterApplyUs { get; init; } = 25_000_000;
    public int MaxSendsPerChunk { get; init; } = 10;
    public long ForeignQuietUs { get; init; } = 10_000_000;
    public long ForeignPauseMaxUs { get; init; } = 60_000_000;
    public long PauseKeepaliveUs { get; init; } = 10_000_000;
    public long RecoverBudgetUs { get; init; } = 55_000_000;
    public long ReopenMaxUs { get; init; } = 30_000_000;
    public long ReopenIntervalUs { get; init; } = 1_000_000;
    public long InfoQueryIntervalUs { get; init; } = 1_000_000;
    public int InfoQueryAttempts { get; init; } = 3;
    public int RecoverRounds { get; init; } = 3;
}

/// <summary>Options of one update (12.5).</summary>
public sealed record UpdateOptions
{
    public BusLoadOptions BusLoad { get; init; } = new();
    public CompressionMode Compression { get; init; } = CompressionMode.Auto;
    public ApplyMode ApplyMode { get; init; } = ApplyMode.Now;
    /// <summary>The firmware OFFER names the release's web UI (5.3.6).</summary>
    public bool IncludeCompanionWebUi { get; init; } = true;
    public bool AllowWebUiVersionMismatch { get; init; }
    /// <summary>At most 16 bytes UTF-8.</summary>
    public string SenderName { get; init; } = "PC Updater";
    public SenderKind SenderKind { get; init; } = SenderKind.StandaloneUpdater;
    /// <summary>Where the deflate cache lives (12.4); null = memory only.</summary>
    public string? CacheDirectory { get; init; }
    public SessionTimers Timers { get; init; } = new();
    /// <summary>Install the firmware of the request (if any).</summary>
    public bool InstallFirmware { get; init; } = true;
    /// <summary>Install the web UI of the request (if any): as companion or on its own.</summary>
    public bool InstallWebUi { get; init; } = true;
}

/// <summary>What to install on one display: a firmware and/or the web UI of the same release.</summary>
public sealed record UpdateRequest
{
    public Images.FirmwareImage? Firmware { get; init; }
    public Images.WebUiImage? WebUi { get; init; }
    /// <summary>
    /// The firmware version the web UI belongs to. Defaults to the firmware's
    /// descriptor version; for a loose web UI file the user enters it (12.4).
    /// </summary>
    public string? WebUiVersion { get; init; }
}

public enum OutcomeKind
{
    /// <summary>Firmware running and validated, or web UI id confirmed.</summary>
    Success,
    /// <summary>ApplyMode None: transferred and verified, not activated.</summary>
    Verified,
    /// <summary>The user chose "Later" on the display; active from the next restart.</summary>
    InstalledRestartPostponed,
    /// <summary>APPLY mode 1 or 2: activated, validation not observed.</summary>
    InstalledNotConfirmed,
    RolledBack,
    Rejected,
    Aborted,
    Failed,
    Unknown,
    /// <summary>Not sent: the sender's own checks refused it (8.2), or nothing to do.</summary>
    Refused,
    /// <summary>Not attempted, because an earlier step did not succeed, or already installed.</summary>
    Skipped,
}

/// <summary>
/// What the sender itself found, as a code a UI can put into words in its own language
/// (<see cref="UpdateOutcome.Message"/> is the English text for logs). Reasons from the
/// display stay in <see cref="UpdateOutcome.Reason"/> (table 4.8); a note says what
/// happened on the sender's side, where a display reason would read the wrong way round
/// (the display's TIMEOUT means "connection to the sender lost").
/// </summary>
public enum SenderNote
{
    None,
    // Failed / Aborted, seen from the sender.
    DisplayRestarted,          // 5.8: the display restarted during the transfer
    SessionDropped,            // the display no longer holds the session
    DisplayUnreachable,        // RECOVER: nothing from the display within 55 s
    NoOfferReply,              // three OFFERs, no answer
    NoResumeReply,             // OFFER RESUME, no answer
    AdapterGone,               // the CAN adapter did not come back within 30 s
    NoResult,                  // no RESULT within 120 s
    NoDecision,                // the dialog ran out and its rejection was lost
    DialogClosed,              // the display closed the dialog without a reason
    BusBusy,                   // another transfer kept the bus busy for 60 s
    ChunkRepeated,             // one chunk sent 10 times (LINK_UNRELIABLE)
    InternalError,
    // Unknown.
    NoActivationConfirmation,  // APPLY mode 1/2 without RESULT
    PollingStopped,            // cancelled while the display validated (H5)
    NoInfo,                    // no INFO after the restart
    NoDecisionInTime,          // 5 minutes without one of the rows of 5.7
    RestartedWithoutNewImage,  // restarted, but neither the new image nor a rollback
    WebUiNotConfirmed,         // the web UI id did not change
    // Refused by the sender's own checks (8.2) and skipped steps.
    LegacyImage,               // 8.6: no descriptor; WiFi or USB
    BadImage,                  // not a usable image or web UI file at all
    WrongBoard,
    TooLarge,
    IncompatibleProtocol,
    NotSupported,              // the display does not take this target
    NothingToInstall,
    WebUiVersionUnknown,       // 12.4: a loose web UI file without a version
    WebUiVersionMismatch,
    Cancelled,
    FirmwareNotInstalled,
    FirmwareActiveAfterRestart,
    WebUiAlreadyInstalled,
    // Good outcomes with a detail.
    NotActivated,              // ApplyMode None
    ActiveFromNextRestart,     // APPLY mode 1
    RestartNotObserved,        // APPLY mode 2
}

/// <summary>The outcome of one target on one display (12.2).</summary>
public sealed record UpdateOutcome
{
    public OutcomeKind Kind { get; init; }
    public Reason Reason { get; init; }
    public bool ByDevice { get; init; }
    public RollbackCause Cause { get; init; }
    public byte Detail { get; init; }
    public InfoMsg? Info { get; init; }
    /// <summary>The sender's own finding; see <see cref="SenderNote"/>.</summary>
    public SenderNote Note { get; init; }
    /// <summary>English, for log files.</summary>
    public string Message { get; init; } = "";

    public bool IsGood => Kind is OutcomeKind.Success or OutcomeKind.Verified or OutcomeKind.InstalledRestartPostponed or OutcomeKind.InstalledNotConfirmed;

    public static UpdateOutcome Of(OutcomeKind k, string message = "") => new() { Kind = k, Message = message };

    public static UpdateOutcome Of(OutcomeKind k, SenderNote note, string message) => new() { Kind = k, Note = note, Message = message };

    public override string ToString()
    {
        string s = Kind.ToString();
        if (Kind is OutcomeKind.Rejected or OutcomeKind.Aborted or OutcomeKind.Failed && Reason != Reason.Ok)
            s += " " + ReasonTexts.Code(Reason) + (Kind == OutcomeKind.Aborted ? (ByDevice ? " by the display" : " by the sender") : "");
        if (Kind == OutcomeKind.RolledBack) s += $" cause {(byte)Cause} ({Cause}), detail {Detail}";
        if (Message.Length > 0) s += ": " + Message;
        return s;
    }
}

/// <summary>The outcome per target of one display.</summary>
public sealed record UpdateReport(DisplayDevice Device, UpdateOutcome? Firmware, UpdateOutcome? WebUi)
{
    public bool IsGood => (Firmware?.IsGood ?? true) && (WebUi is null || WebUi.IsGood || WebUi.Kind == OutcomeKind.Skipped);
}

/// <summary>Sender phases as the UI shows them.</summary>
public enum SenderPhase
{
    Offering,
    WaitingForConfirmation,
    Preparing,
    Transferring,
    PausedForeignTransfer,
    Verifying,
    Applying,
    WaitingForValidation,
    CheckingWebUi,
    Recovering,
    Done,
}

/// <summary>A progress report (12.7): phase, percentage, ETA, bus loads.</summary>
public sealed record UpdateProgress
{
    public UpdateTarget Target { get; init; }
    public SenderPhase Phase { get; init; }
    public uint StreamAccepted { get; init; }
    public uint StreamSize { get; init; }
    public double Fraction => StreamSize == 0 ? 0 : (double)StreamAccepted / StreamSize;
    public double? EtaSeconds { get; init; }
    /// <summary>Seconds left in the display's confirmation dialog.</summary>
    public int? DialogSecondsLeft { get; init; }
    /// <summary>Seconds until the display's OtaGuard decides.</summary>
    public int? VerifySecondsLeft { get; init; }
    public double OwnLoad { get; init; }
    public double TotalLoad { get; init; }
    public double EffectiveShare { get; init; }
    /// <summary>
    /// True while the display checks a new firmware that needs this sender's polls
    /// (APPLY mode 0, H5): closing now makes the display roll back (12.7).
    /// </summary>
    public bool MustStayConnected { get; init; }
    public string Message { get; init; } = "";
}
