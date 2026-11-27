using System.Buffers.Binary;

namespace NauticPinnace.N2kUpdate.Protocol;

/// <summary>
/// One message of the update protocol (section 3). Pure encode and decode,
/// checked byte for byte against Appendix A.
/// </summary>
public abstract record UpdateMessage
{
    public abstract MsgType Type { get; }

    /// <summary>Header byte 4 (byte 3 of DATA): 1..254 for a session, 0 for none.</summary>
    public byte SessionId { get; init; }

    /// <summary>Header byte 3 of control messages. DATA carries none (3.5).</summary>
    public byte ProtocolVersion { get; init; } = UpdProto.Version;

    public abstract byte[] Encode();

    /// <summary>True for the types a sender sends to a display.</summary>
    public static bool IsSenderToDisplay(MsgType t) =>
        t is MsgType.Query or MsgType.Offer or MsgType.Data or MsgType.Apply or MsgType.Abort or MsgType.Identify;

    /// <summary>True for the types a display sends to a sender.</summary>
    public static bool IsDisplayToSender(MsgType t) =>
        t is MsgType.Info or MsgType.OfferReply or MsgType.Ack or MsgType.Result or MsgType.Abort or MsgType.Status;

    /// <summary>
    /// Decodes a reassembled PGN 126720 payload. Returns null for anything that is
    /// not this protocol: another manufacturer header, an unknown type, or a message
    /// shorter than its v1 length (2.2, 2.5). Messages longer than the v1 length are
    /// accepted and the extra bytes ignored (3.0). The protocol version byte is
    /// reported, not judged: the caller decides (5.1 rule 3).
    /// </summary>
    public static UpdateMessage? Decode(ReadOnlySpan<byte> p)
    {
        if (p.Length < 4 || p[0] != UpdProto.Mfg0 || p[1] != UpdProto.Mfg1) return null;
        var type = (MsgType)p[2];
        if (type < MsgType.Query || type > MsgType.Identify) return null;
        if (p.Length < UpdProto.MinLength(type)) return null;
        return type switch
        {
            MsgType.Query      => QueryMsg.DecodeBody(p),
            MsgType.Info       => InfoMsg.DecodeBody(p),
            MsgType.Offer      => OfferMsg.DecodeBody(p),
            MsgType.OfferReply => OfferReplyMsg.DecodeBody(p),
            MsgType.Data       => DataMsg.DecodeBody(p),
            MsgType.Ack        => AckMsg.DecodeBody(p),
            MsgType.Result     => ResultMsg.DecodeBody(p),
            MsgType.Apply      => ApplyMsg.DecodeBody(p),
            MsgType.Abort      => AbortMsg.DecodeBody(p),
            MsgType.Status     => StatusMsg.DecodeBody(p),
            MsgType.Identify   => IdentifyMsg.DecodeBody(p),
            _                  => null,
        };
    }

    /// <summary>Writes the control-message header H (3.0) and returns the buffer.</summary>
    protected byte[] NewControl(int length)
    {
        var b = new byte[length];
        b[0] = UpdProto.Mfg0;
        b[1] = UpdProto.Mfg1;
        b[2] = (byte)Type;
        b[3] = ProtocolVersion;
        b[4] = SessionId;
        return b;
    }
}

/// <summary>QUERY (0xA0), sender to display or 255, 12 bytes (3.1).</summary>
public sealed record QueryMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Query;
    public QueryWhat What { get; init; }
    public UniqueId UidFilter { get; init; } = UniqueId.Any;

    public override byte[] Encode()
    {
        var b = NewControl(12);
        b[5] = (byte)What;
        UidFilter.Write(b.AsSpan(6));
        return b;
    }

    internal static QueryMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4],
        What = (QueryWhat)p[5], UidFilter = UniqueId.Read(p[6..]),
    };
}

/// <summary>INFO (0xA1), display to sender, 129 bytes (3.2).</summary>
public sealed record InfoMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Info;
    public byte ProtoMin { get; init; } = 1;
    public byte ProtoMax { get; init; } = 1;
    public BoardId Board { get; init; }
    public Capabilities Caps { get; init; }
    public UniqueId Uid { get; init; }
    public SessionState SessionState { get; init; }
    public OtaState OtaState { get; init; }
    /// <summary>Seconds until OtaGuard decides; 0xFFFF = not verifying.</summary>
    public ushort VerifySecondsLeft { get; init; } = 0xFFFF;
    /// <summary>0 = app0, 1 = app1, 0xFF = unknown.</summary>
    public byte RunningSlot { get; init; } = 0xFF;
    public StatusFlags Flags { get; init; }
    public uint MaxFirmwareSize { get; init; }
    public uint MaxWebUiSize { get; init; }
    public byte MaxChunkSize { get; init; } = UpdProto.ChunkSize;
    public byte MaxWindowChunks { get; init; } = UpdProto.MaxWindowChunks;
    public string RunningVersion { get; init; } = "";
    public string LastInvalidVersion { get; init; } = "";
    public string Label { get; init; } = "";
    public ImageId RunningImageId { get; init; }
    public ImageId LastInvalidImageId { get; init; }
    public RollbackCause RollbackCause { get; init; }
    public byte RollbackDetail { get; init; }
    public string WebUiVersion { get; init; } = "";
    public ImageId WebUiId { get; init; }
    public uint WebUiSize { get; init; }
    public uint UptimeSeconds { get; init; }

    public override byte[] Encode()
    {
        var b = NewControl(129);
        var s = b.AsSpan();
        b[5] = ProtoMin; b[6] = ProtoMax; b[7] = (byte)Board; b[8] = (byte)Caps;
        Uid.Write(s[9..]);
        b[15] = (byte)SessionState; b[16] = (byte)OtaState;
        BinaryPrimitives.WriteUInt16LittleEndian(s[17..], VerifySecondsLeft);
        b[19] = RunningSlot; b[20] = (byte)Flags;
        BinaryPrimitives.WriteUInt32LittleEndian(s[21..], MaxFirmwareSize);
        BinaryPrimitives.WriteUInt32LittleEndian(s[25..], MaxWebUiSize);
        b[29] = MaxChunkSize; b[30] = MaxWindowChunks;
        TextField.WriteVersion(s[31..], RunningVersion);
        TextField.WriteVersion(s[47..], LastInvalidVersion);
        TextField.WriteUtf8(s[63..], Label);
        RunningImageId.Write(s[79..]);
        LastInvalidImageId.Write(s[87..]);
        b[95] = (byte)RollbackCause; b[96] = RollbackDetail;
        TextField.WriteVersion(s[97..], WebUiVersion);
        WebUiId.Write(s[113..]);
        BinaryPrimitives.WriteUInt32LittleEndian(s[121..], WebUiSize);
        BinaryPrimitives.WriteUInt32LittleEndian(s[125..], UptimeSeconds);
        return b;
    }

    internal static InfoMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4],
        ProtoMin = p[5], ProtoMax = p[6], Board = (BoardId)p[7], Caps = (Capabilities)p[8],
        Uid = UniqueId.Read(p[9..]),
        SessionState = (SessionState)p[15], OtaState = (OtaState)p[16],
        VerifySecondsLeft = BinaryPrimitives.ReadUInt16LittleEndian(p[17..]),
        RunningSlot = p[19], Flags = (StatusFlags)p[20],
        MaxFirmwareSize = BinaryPrimitives.ReadUInt32LittleEndian(p[21..]),
        MaxWebUiSize = BinaryPrimitives.ReadUInt32LittleEndian(p[25..]),
        MaxChunkSize = p[29], MaxWindowChunks = p[30],
        RunningVersion = TextField.Read(p.Slice(31, 16)),
        LastInvalidVersion = TextField.Read(p.Slice(47, 16)),
        Label = TextField.Read(p.Slice(63, 16)),
        RunningImageId = ImageId.Read(p[79..]),
        LastInvalidImageId = ImageId.Read(p[87..]),
        RollbackCause = (RollbackCause)p[95], RollbackDetail = p[96],
        WebUiVersion = TextField.Read(p.Slice(97, 16)),
        WebUiId = ImageId.Read(p[113..]),
        WebUiSize = BinaryPrimitives.ReadUInt32LittleEndian(p[121..]),
        UptimeSeconds = BinaryPrimitives.ReadUInt32LittleEndian(p[125..]),
    };
}

/// <summary>OFFER (0xA2), sender to display, 131 bytes (3.3).</summary>
public sealed record OfferMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Offer;
    public UpdateTarget Target { get; init; }
    public BoardId Board { get; init; }
    public StreamEncoding Encoding { get; init; }
    public OfferFlags Flags { get; init; }
    public byte ChunkSize { get; init; } = UpdProto.ChunkSize;
    public uint ImageSize { get; init; }
    public uint StreamSize { get; init; }
    public byte[] Sha256 { get; init; } = new byte[32];
    public UniqueId TargetUid { get; init; }
    public string Version { get; init; } = "";
    public string SenderName { get; init; } = "";
    public SenderKind SenderKind { get; init; }
    public ushort EstimatedSeconds { get; init; } = 0xFFFF;
    public uint StreamCrc32 { get; init; }
    public byte[] CompanionSha256 { get; init; } = new byte[32];
    public uint CompanionSize { get; init; }

    public override byte[] Encode()
    {
        var b = NewControl(131);
        var s = b.AsSpan();
        b[5] = (byte)Target; b[6] = (byte)Board; b[7] = (byte)Encoding; b[8] = (byte)Flags; b[9] = ChunkSize;
        BinaryPrimitives.WriteUInt32LittleEndian(s[10..], ImageSize);
        BinaryPrimitives.WriteUInt32LittleEndian(s[14..], StreamSize);
        Sha256.AsSpan(0, 32).CopyTo(s[18..]);
        TargetUid.Write(s[50..]);
        TextField.WriteVersion(s[56..], Version);
        TextField.WriteUtf8(s[72..], SenderName);
        b[88] = (byte)SenderKind;
        BinaryPrimitives.WriteUInt16LittleEndian(s[89..], EstimatedSeconds);
        BinaryPrimitives.WriteUInt32LittleEndian(s[91..], StreamCrc32);
        CompanionSha256.AsSpan(0, 32).CopyTo(s[95..]);
        BinaryPrimitives.WriteUInt32LittleEndian(s[127..], CompanionSize);
        return b;
    }

    internal static OfferMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4],
        Target = (UpdateTarget)p[5], Board = (BoardId)p[6], Encoding = (StreamEncoding)p[7],
        Flags = (OfferFlags)p[8], ChunkSize = p[9],
        ImageSize = BinaryPrimitives.ReadUInt32LittleEndian(p[10..]),
        StreamSize = BinaryPrimitives.ReadUInt32LittleEndian(p[14..]),
        Sha256 = p.Slice(18, 32).ToArray(),
        TargetUid = UniqueId.Read(p[50..]),
        Version = TextField.Read(p.Slice(56, 16)),
        SenderName = TextField.Read(p.Slice(72, 16)),
        SenderKind = (SenderKind)p[88],
        EstimatedSeconds = BinaryPrimitives.ReadUInt16LittleEndian(p[89..]),
        StreamCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(p[91..]),
        CompanionSha256 = p.Slice(95, 32).ToArray(),
        CompanionSize = BinaryPrimitives.ReadUInt32LittleEndian(p[127..]),
    };

    public bool HasCompanion => CompanionSize != 0;
}

/// <summary>OFFER_REPLY (0xA3), display to sender, 10 bytes (3.4).</summary>
public sealed record OfferReplyMsg : UpdateMessage
{
    public override MsgType Type => MsgType.OfferReply;
    public OfferDecision Decision { get; init; }
    public Reason Reason { get; init; }
    public ushort Seconds { get; init; }
    public byte WindowChunks { get; init; }

    public override byte[] Encode()
    {
        var b = NewControl(10);
        b[5] = (byte)Decision; b[6] = (byte)Reason;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(7), Seconds);
        b[9] = WindowChunks;
        return b;
    }

    internal static OfferReplyMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4],
        Decision = (OfferDecision)p[5], Reason = (Reason)p[6],
        Seconds = BinaryPrimitives.ReadUInt16LittleEndian(p[7..]), WindowChunks = p[9],
    };
}

/// <summary>DATA (0xA4), sender to display, 7 + n bytes (3.5). No version byte.</summary>
public sealed record DataMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Data;
    /// <summary>Byte offset in the stream, a multiple of 216 (u24).</summary>
    public uint Offset { get; init; }
    public ReadOnlyMemory<byte> Data { get; init; }

    public override byte[] Encode() => Build(SessionId, Offset, Data.Span);

    /// <summary>Builds a DATA payload without an intermediate record (hot path).</summary>
    public static byte[] Build(byte session, uint offset, ReadOnlySpan<byte> data)
    {
        var b = new byte[UpdProto.DataHeaderLength + data.Length];
        b[0] = UpdProto.Mfg0; b[1] = UpdProto.Mfg1; b[2] = (byte)MsgType.Data; b[3] = session;
        b[4] = (byte)offset; b[5] = (byte)(offset >> 8); b[6] = (byte)(offset >> 16);
        data.CopyTo(b.AsSpan(UpdProto.DataHeaderLength));
        return b;
    }

    internal static DataMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = 0, SessionId = p[3],
        Offset = (uint)(p[4] | (p[5] << 8) | (p[6] << 16)),
        Data = p[UpdProto.DataHeaderLength..].ToArray(),
    };
}

/// <summary>ACK (0xA5), display to sender, 28 bytes (3.6).</summary>
public sealed record AckMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Ack;
    /// <summary>3 = RECEIVING (credit) or 4 = PROCESSING (no credit).</summary>
    public SessionState State { get; init; }
    public uint Base { get; init; }
    public uint End { get; init; }
    /// <summary>Bit i set: the chunk at base + i × 216 has arrived.</summary>
    public ulong Bitmap { get; init; }
    public uint StreamCrc32 { get; init; }
    public ushort HoldMs { get; init; }

    public bool HasCredit => End > Base;

    public override byte[] Encode()
    {
        var b = NewControl(28);
        var s = b.AsSpan();
        b[5] = (byte)State;
        BinaryPrimitives.WriteUInt32LittleEndian(s[6..], Base);
        BinaryPrimitives.WriteUInt32LittleEndian(s[10..], End);
        BinaryPrimitives.WriteUInt64LittleEndian(s[14..], Bitmap);
        BinaryPrimitives.WriteUInt32LittleEndian(s[22..], StreamCrc32);
        BinaryPrimitives.WriteUInt16LittleEndian(s[26..], HoldMs);
        return b;
    }

    internal static AckMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4], State = (SessionState)p[5],
        Base = BinaryPrimitives.ReadUInt32LittleEndian(p[6..]),
        End = BinaryPrimitives.ReadUInt32LittleEndian(p[10..]),
        Bitmap = BinaryPrimitives.ReadUInt64LittleEndian(p[14..]),
        StreamCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(p[22..]),
        HoldMs = BinaryPrimitives.ReadUInt16LittleEndian(p[26..]),
    };
}

/// <summary>RESULT (0xA6), display to sender, 40 bytes (3.7).</summary>
public sealed record ResultMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Result;
    public Reason Code { get; init; }
    public ResultPhase Phase { get; init; }
    public byte Detail { get; init; }
    public byte[] Sha256 { get; init; } = new byte[32];

    public override byte[] Encode()
    {
        var b = NewControl(40);
        b[5] = (byte)Code; b[6] = (byte)Phase; b[7] = Detail;
        Sha256.AsSpan(0, 32).CopyTo(b.AsSpan(8));
        return b;
    }

    internal static ResultMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4],
        Code = (Reason)p[5], Phase = (ResultPhase)p[6], Detail = p[7],
        Sha256 = p.Slice(8, 32).ToArray(),
    };
}

/// <summary>APPLY (0xA7), sender to display, 6 bytes (3.8).</summary>
public sealed record ApplyMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Apply;
    public ApplyWireMode Mode { get; init; }

    public override byte[] Encode()
    {
        var b = NewControl(6);
        b[5] = (byte)Mode;
        return b;
    }

    internal static ApplyMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4], Mode = (ApplyWireMode)p[5],
    };
}

/// <summary>ABORT (0xA8), either direction, 6 bytes (3.9). Never acknowledged.</summary>
public sealed record AbortMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Abort;
    public Reason Reason { get; init; }

    public override byte[] Encode()
    {
        var b = NewControl(6);
        b[5] = (byte)Reason;
        return b;
    }

    internal static AbortMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4], Reason = (Reason)p[5],
    };
}

/// <summary>STATUS (0xA9), display to sender, 25 bytes (3.10).</summary>
public sealed record StatusMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Status;
    public SessionState State { get; init; }
    public Reason Reason { get; init; }
    public uint StreamAccepted { get; init; }
    public uint StreamSize { get; init; }
    public uint ImageWritten { get; init; }
    public uint ImageSize { get; init; }
    /// <summary>Seconds left in the current phase; 0xFFFF = NA.</summary>
    public ushort Seconds { get; init; } = 0xFFFF;

    public override byte[] Encode()
    {
        var b = NewControl(25);
        var s = b.AsSpan();
        b[5] = (byte)State; b[6] = (byte)Reason;
        BinaryPrimitives.WriteUInt32LittleEndian(s[7..], StreamAccepted);
        BinaryPrimitives.WriteUInt32LittleEndian(s[11..], StreamSize);
        BinaryPrimitives.WriteUInt32LittleEndian(s[15..], ImageWritten);
        BinaryPrimitives.WriteUInt32LittleEndian(s[19..], ImageSize);
        BinaryPrimitives.WriteUInt16LittleEndian(s[23..], Seconds);
        return b;
    }

    internal static StatusMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4],
        State = (SessionState)p[5], Reason = (Reason)p[6],
        StreamAccepted = BinaryPrimitives.ReadUInt32LittleEndian(p[7..]),
        StreamSize = BinaryPrimitives.ReadUInt32LittleEndian(p[11..]),
        ImageWritten = BinaryPrimitives.ReadUInt32LittleEndian(p[15..]),
        ImageSize = BinaryPrimitives.ReadUInt32LittleEndian(p[19..]),
        Seconds = BinaryPrimitives.ReadUInt16LittleEndian(p[23..]),
    };
}

/// <summary>IDENTIFY (0xAA), sender to display, 12 bytes (3.11). No reply.</summary>
public sealed record IdentifyMsg : UpdateMessage
{
    public override MsgType Type => MsgType.Identify;
    /// <summary>1..30; larger values count as 30.</summary>
    public byte Seconds { get; init; }
    public UniqueId TargetUid { get; init; }

    public override byte[] Encode()
    {
        var b = NewControl(12);
        b[5] = Seconds;
        TargetUid.Write(b.AsSpan(6));
        return b;
    }

    internal static IdentifyMsg DecodeBody(ReadOnlySpan<byte> p) => new()
    {
        ProtocolVersion = p[3], SessionId = p[4], Seconds = p[5], TargetUid = UniqueId.Read(p[6..]),
    };
}
