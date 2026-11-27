namespace NauticPinnace.N2kUpdate.Protocol;

/// <summary>
/// Every protocol constant of docs/N2K-UPDATE.md (protocol version 1, revision 2),
/// written down once. The device side keeps its copy in N2kUpdateCore.h.
/// </summary>
public static class UpdProto
{
    /// <summary>Addressed proprietary fast-packet PGN (2.1).</summary>
    public const int Pgn = 126720;

    /// <summary>Manufacturer header, little endian: (4 &lt;&lt; 13) | (3 &lt;&lt; 11) | 2046 (2.2).</summary>
    public const byte Mfg0 = 0xFE, Mfg1 = 0x9F;

    public const int ManufacturerCode = 2046;

    /// <summary>The one protocol version this library speaks (13).</summary>
    public const byte Version = 1;
    public const byte VersionMin = 1, VersionMax = 1;

    /// <summary>Fixed chunk size in v1 (5.4, 15.1).</summary>
    public const int ChunkSize = 216;

    /// <summary>Largest window a v1 display grants (INFO byte 30).</summary>
    public const int MaxWindowChunks = 64;

    /// <summary>Largest fast-packet payload (32 frames, 2.3).</summary>
    public const int MaxPayload = 223;

    /// <summary>Header length of a DATA message (manufacturer, type, session, u24 offset).</summary>
    public const int DataHeaderLength = 7;

    public const int PrioControl = 6;
    public const int PrioData    = 7;

    /// <summary>Destination for a broadcast QUERY (only QUERY may be broadcast).</summary>
    public const int Broadcast = 255;

    /// <summary>Highest stream size a u24 DATA offset can address (3.3).</summary>
    public const uint MaxStreamSize = 0xFFFFFF;

    /// <summary>v1 lengths of every message type (2.5). Longer messages are accepted (3.0).</summary>
    public static int MinLength(MsgType t) => t switch
    {
        MsgType.Query      => 12,
        MsgType.Info       => 129,
        MsgType.Offer      => 131,
        MsgType.OfferReply => 10,
        MsgType.Data       => DataHeaderLength + 1,
        MsgType.Ack        => 28,
        MsgType.Result     => 40,
        MsgType.Apply      => 6,
        MsgType.Abort      => 6,
        MsgType.Status     => 25,
        MsgType.Identify   => 12,
        _                  => int.MaxValue,
    };

    /// <summary>Upper bound of a deflate stream for a given image size (3.3).</summary>
    public static long StreamBound(long imageSize) => imageSize + imageSize / 1000 + 1024;
}

/// <summary>Message types (2.5).</summary>
public enum MsgType : byte
{
    Query      = 0xA0,
    Info       = 0xA1,
    Offer      = 0xA2,
    OfferReply = 0xA3,
    Data       = 0xA4,
    Ack        = 0xA5,
    Result     = 0xA6,
    Apply      = 0xA7,
    Abort      = 0xA8,
    Status     = 0xA9,
    Identify   = 0xAA,
}

/// <summary>QUERY "what" (4.12).</summary>
public enum QueryWhat : byte { Info = 1, Status = 2, Ack = 3, Result = 4 }

/// <summary>Board ids (4.1). Only ever added, never renumbered.</summary>
public enum BoardId : byte { Any = 0, Inch4 = 1, B7 = 2, B5 = 3 }

/// <summary>Update targets (4.2).</summary>
public enum UpdateTarget : byte { Firmware = 1, WebUi = 2 }

/// <summary>Stream encodings (4.3).</summary>
public enum StreamEncoding : byte { Raw = 0, Deflate = 1 }

/// <summary>INFO byte 8 (4.4).</summary>
[Flags]
public enum Capabilities : byte
{
    None             = 0,
    Firmware         = 1 << 0,
    WebUi            = 1 << 1,
    Deflate          = 1 << 2,
    Resume           = 1 << 3,
    Identify         = 1 << 4,
    CompanionWebUi   = 1 << 5,
}

/// <summary>INFO byte 20 (4.5).</summary>
[Flags]
public enum StatusFlags : byte
{
    None               = 0,
    AcceptUpdates      = 1 << 0,
    AlarmActive        = 1 << 1,
    RestartPending     = 1 << 2,
    RolledBack         = 1 << 3,
    Ready              = 1 << 4,
    CompanionApproved  = 1 << 5,
    WifiOtaInProgress  = 1 << 6,
}

/// <summary>INFO byte 16 (4.6).</summary>
public enum OtaState : byte { NotAvailable = 0, Valid = 1, PendingVerify = 2 }

/// <summary>Session states of the display (4.7).</summary>
public enum SessionState : byte
{
    Idle         = 0,
    Confirming   = 1,
    Preparing    = 2,
    Receiving    = 3,
    Processing   = 4,
    Verifying    = 5,
    ReadyToApply = 6,
    Applying     = 7,
    Done         = 8,
    Failed       = 9,
}

/// <summary>Reason and result codes (4.8). Unknown codes are shown by number.</summary>
public enum Reason : byte
{
    Ok                   = 0x00,
    DeclinedByUser       = 0x01,
    ConfirmTimeout       = 0x02,
    Disabled             = 0x03,
    Busy                 = 0x04,
    WrongBoard           = 0x05,
    TooLarge             = 0x06,
    UnsupportedTarget    = 0x07,
    UnsupportedEncoding  = 0x08,
    UnsupportedProtocol  = 0x09,
    BadRequest           = 0x0A,
    PendingVerify        = 0x0B,
    AlarmActive          = 0x0C,
    WrongDevice          = 0x0D,
    VersionMismatch      = 0x0E,
    NoSpace              = 0x0F,
    OutOfMemory          = 0x10,
    DescriptorInvalid    = 0x11,
    ShaMismatch          = 0x12,
    ImageInvalid         = 0x13,
    SizeMismatch         = 0x14,
    FlashError           = 0x15,
    Timeout              = 0x16,
    UserCancelled        = 0x17,
    SenderCancelled      = 0x18,
    SessionUnknown       = 0x19,
    StreamCrcMismatch    = 0x1A,
    LinkUnreliable       = 0x1B,
    ApplyFailed          = 0x1C,
    RestartPending       = 0x1D,
    NotReady             = 0x1E,
    LowVoltage           = 0x1F,
    InternalError        = 0xFE,
}

/// <summary>OFFER_REPLY decision (3.4).</summary>
public enum OfferDecision : byte { PendingUser = 0, Accepted = 1, Rejected = 2 }

/// <summary>RESULT phase (3.7).</summary>
public enum ResultPhase : byte { Verified = 1, Applied = 2, Activated = 3 }

/// <summary>APPLY mode on the wire (3.8).</summary>
public enum ApplyWireMode : byte { Now = 0, NextRestart = 1, NowNoWait = 2 }

/// <summary>OFFER flags (4.10).</summary>
[Flags]
public enum OfferFlags : byte
{
    None                 = 0,
    Resume               = 1 << 0,
    AllowVersionMismatch = 1 << 1,
}

/// <summary>Sender kind (4.9).</summary>
public enum SenderKind : byte { Other = 0, NauticPi = 1, StandaloneUpdater = 2 }

/// <summary>Rollback cause, INFO byte 95 (4.11).</summary>
public enum RollbackCause : byte
{
    None         = 0,
    HealthCheck  = 1,
    Crash        = 2,
    PowerLoss    = 3,
    OtherRestart = 4,
    CrashLoop    = 5,
}

/// <summary>Panel geometry and FW_BOARD_NAME per board id (4.1).</summary>
public static class Boards
{
    public static string? BoardName(BoardId b) => b switch
    {
        BoardId.Inch4 => "waveshare-4inch-480x480",
        BoardId.B7    => "waveshare-7b-1024x600",
        BoardId.B5    => "waveshare-5b-1024x600",
        _             => null,
    };

    public static (int W, int H)? Panel(BoardId b) => b switch
    {
        BoardId.Inch4 => (480, 480),
        BoardId.B7    => (1024, 600),
        BoardId.B5    => (1024, 600),
        _             => null,
    };

    /// <summary>Release-folder name of a board (the layout of the release ZIP: 4/, 7b/, 5b/).</summary>
    public static string? FolderName(BoardId b) => b switch
    {
        BoardId.Inch4 => "4",
        BoardId.B7    => "7b",
        BoardId.B5    => "5b",
        _             => null,
    };

    /// <summary>Short human name for lists and logs.</summary>
    public static string DisplayName(BoardId b) => b switch
    {
        BoardId.Inch4 => "4-inch",
        BoardId.B7    => "7B",
        BoardId.B5    => "5B",
        BoardId.Any   => "any",
        _             => $"board {(byte)b}",
    };

    public static readonly BoardId[] Known = [BoardId.Inch4, BoardId.B7, BoardId.B5];
}
