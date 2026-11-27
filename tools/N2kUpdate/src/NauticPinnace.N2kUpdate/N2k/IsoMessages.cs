using System.Buffers.Binary;
using System.Text;

namespace NauticPinnace.N2kUpdate.N2k;

/// <summary>Standard NMEA 2000 network messages the sender needs (12.3, 5.2).</summary>
public static class N2kPgn
{
    public const int IsoRequest      = 59904;   // single frame, 3 bytes
    public const int IsoAddressClaim = 60928;   // single frame, 8 bytes = NAME
    public const int ProductInfo     = 126996;  // fast packet, 134 bytes
    public const int ConfigInfo      = 126998;  // fast packet
    public const int PgnList         = 126464;  // fast packet

    /// <summary>The fast-packet PGNs the sender reassembles; everything else is single frame.</summary>
    public static bool IsFastPacket(int pgn) =>
        pgn is Protocol.UpdProto.Pgn or ProductInfo or ConfigInfo or PgnList;

    /// <summary>The null address, used for "cannot claim" (J1939-81).</summary>
    public const int NullAddress = 254;
}

/// <summary>
/// The 64-bit ISO NAME (PGN 60928). Lower values win address contests. Layout as in
/// the address claim of the author's own earlier, unpublished slcan sender:
///   bits 0-20 unique number, 21-31 manufacturer code, 32-34 device instance lower,
///   35-39 device instance upper, 40-47 device function, 48 reserved,
///   49-55 device class, 56-59 system instance, 60-62 industry group,
///   63 arbitrary address capable.
/// </summary>
public readonly record struct N2kName(ulong Value)
{
    public static N2kName Create(int uniqueNumber, int manufacturerCode, int deviceFunction, int deviceClass,
                                 int industryGroup = 4, int deviceInstance = 0, int systemInstance = 0,
                                 bool arbitraryAddressCapable = true)
    {
        ulong v = 0;
        v |= (ulong)(uint)(uniqueNumber & 0x1FFFFF);
        v |= (ulong)(uint)(manufacturerCode & 0x7FF) << 21;
        v |= (ulong)(uint)(deviceInstance & 0xFF) << 32;
        v |= (ulong)(uint)(deviceFunction & 0xFF) << 40;
        v |= (ulong)(uint)(deviceClass & 0x7F) << 49;
        v |= (ulong)(uint)(systemInstance & 0x0F) << 56;
        v |= (ulong)(uint)(industryGroup & 0x07) << 60;
        if (arbitraryAddressCapable) v |= 1UL << 63;
        return new N2kName(v);
    }

    /// <summary>
    /// The sender's NAME (12.3): the stored 21-bit unique number, manufacturer 2046,
    /// device class 25 (inter/intranetwork device) and function 130 (PC gateway),
    /// industry group 4, arbitrary-address capable.
    /// </summary>
    public static N2kName ForUpdater(int uniqueNumber) =>
        Create(uniqueNumber, Protocol.UpdProto.ManufacturerCode, deviceFunction: 130, deviceClass: 25);

    public int UniqueNumber => (int)(Value & 0x1FFFFF);
    public int ManufacturerCode => (int)((Value >> 21) & 0x7FF);
    public int DeviceFunction => (int)((Value >> 40) & 0xFF);
    public int DeviceClass => (int)((Value >> 49) & 0x7F);
    public int IndustryGroup => (int)((Value >> 60) & 0x07);
    public bool ArbitraryAddressCapable => (Value >> 63) != 0;

    public byte[] ToBytes()
    {
        var b = new byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(b, Value);
        return b;
    }

    public static N2kName Read(ReadOnlySpan<byte> s) => new(BinaryPrimitives.ReadUInt64LittleEndian(s));

    public override string ToString() => Value.ToString("X16");
}

/// <summary>PGN 126996 product information (134 bytes, fast packet).</summary>
public sealed record ProductInfo
{
    public ushort N2kDbVersion { get; init; } = 2100;
    public ushort ProductCode { get; init; }
    public string ModelId { get; init; } = "";
    public string SoftwareVersion { get; init; } = "";
    public string ModelVersion { get; init; } = "";
    public string SerialCode { get; init; } = "";
    public byte CertificationLevel { get; init; }
    public byte LoadEquivalency { get; init; } = 1;

    public byte[] Encode()
    {
        var b = new byte[134];
        BinaryPrimitives.WriteUInt16LittleEndian(b, N2kDbVersion);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(2), ProductCode);
        WriteStr(b.AsSpan(4, 32), ModelId);
        WriteStr(b.AsSpan(36, 32), SoftwareVersion);
        WriteStr(b.AsSpan(68, 32), ModelVersion);
        WriteStr(b.AsSpan(100, 32), SerialCode);
        b[132] = CertificationLevel;
        b[133] = LoadEquivalency;
        return b;
    }

    public static ProductInfo? Decode(ReadOnlySpan<byte> p)
    {
        if (p.Length < 134) return null;
        return new ProductInfo
        {
            N2kDbVersion = BinaryPrimitives.ReadUInt16LittleEndian(p),
            ProductCode = BinaryPrimitives.ReadUInt16LittleEndian(p[2..]),
            ModelId = ReadStr(p.Slice(4, 32)),
            SoftwareVersion = ReadStr(p.Slice(36, 32)),
            ModelVersion = ReadStr(p.Slice(68, 32)),
            SerialCode = ReadStr(p.Slice(100, 32)),
            CertificationLevel = p[132],
            LoadEquivalency = p[133],
        };
    }

    // ASCII, padded with 0xFF like the NMEA2000 library's AddStr. Readers strip
    // 0x00, 0xFF, '@' and trailing spaces, so any of the usual paddings decodes.
    private static void WriteStr(Span<byte> d, string s)
    {
        d.Fill(0xFF);
        for (int i = 0; i < Math.Min(d.Length, s.Length); i++)
            d[i] = s[i] >= 0x20 && s[i] < 0x7F ? (byte)s[i] : (byte)'?';
    }

    private static string ReadStr(ReadOnlySpan<byte> s)
    {
        int n = 0;
        while (n < s.Length && s[n] != 0x00 && s[n] != 0xFF) n++;
        return Encoding.ASCII.GetString(s[..n]).TrimEnd(' ', '@');
    }
}
