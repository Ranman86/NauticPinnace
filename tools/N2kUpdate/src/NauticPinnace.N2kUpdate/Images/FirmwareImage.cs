using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Images;

/// <summary>The 64-byte firmware descriptor <c>np_fw_desc_t</c> at image offset 0x120 (8.1).</summary>
public sealed record FwDescriptor
{
    public const int Offset = 0x120;
    public const int Size = 64;
    public const byte FlagOtaGuard = 0x01;
    public static ReadOnlySpan<byte> Magic => "NPFWDESC"u8;

    public byte DescVersion { get; init; } = 1;
    public BoardId Board { get; init; }
    public ushort DescSize { get; init; } = Size;
    public ushort PanelWidth { get; init; }
    public ushort PanelHeight { get; init; }
    public byte N2kUpdateProto { get; init; } = 1;
    public byte Flags { get; init; } = FlagOtaGuard;
    public string FwVersion { get; init; } = "";
    public string BoardName { get; init; } = "";

    public bool OtaGuard => (Flags & FlagOtaGuard) != 0;

    public static bool HasMagic(ReadOnlySpan<byte> image) =>
        image.Length >= Offset + Size && image.Slice(Offset, 8).SequenceEqual(Magic);

    public static FwDescriptor Parse(ReadOnlySpan<byte> d) => new()
    {
        DescVersion = d[8],
        Board = (BoardId)d[9],
        DescSize = BinaryPrimitives.ReadUInt16LittleEndian(d[10..]),
        PanelWidth = BinaryPrimitives.ReadUInt16LittleEndian(d[12..]),
        PanelHeight = BinaryPrimitives.ReadUInt16LittleEndian(d[14..]),
        N2kUpdateProto = d[16],
        Flags = d[17],
        FwVersion = ReadAscii(d.Slice(0x14, 16)),
        BoardName = ReadAscii(d.Slice(0x24, 28)),
    };

    public byte[] Encode()
    {
        var b = new byte[Size];
        Magic.CopyTo(b);
        b[8] = DescVersion;
        b[9] = (byte)Board;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(10), DescSize);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(12), PanelWidth);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(14), PanelHeight);
        b[16] = N2kUpdateProto;
        b[17] = Flags;
        Encoding.ASCII.GetBytes(FwVersion.AsSpan(0, Math.Min(15, FwVersion.Length)), b.AsSpan(0x14));
        Encoding.ASCII.GetBytes(BoardName.AsSpan(0, Math.Min(27, BoardName.Length)), b.AsSpan(0x24));
        return b;
    }

    private static string ReadAscii(ReadOnlySpan<byte> s)
    {
        int n = s.IndexOf((byte)0);
        if (n < 0) n = s.Length;
        return Encoding.ASCII.GetString(s[..n]);
    }
}

/// <summary>
/// A firmware.bin loaded into memory once and hashed once (12.4). The file is never
/// read again during a session, so a rebuild mid-transfer cannot mix two images.
/// </summary>
public sealed class FirmwareImage
{
    public const uint AppDescMagic = 0xABCD5432;
    public const int ImageIdOffset = 0xB0;

    public byte[] Bytes { get; }
    public string Source { get; }
    public byte[] Sha256 { get; }
    public uint Crc32 { get; }

    /// <summary>The descriptor, or null for a legacy image (8.6).</summary>
    public FwDescriptor? Descriptor { get; }

    /// <summary>First 8 bytes of app_elf_sha256 at 0xB0 (8.3); None if the app description is missing.</summary>
    public ImageId ImageId { get; }

    /// <summary>Structural problems that make the file unusable over N2K for ANY display.</summary>
    public IReadOnlyList<string> Problems { get; }

    /// <summary>For a legacy image: the board named by exactly one FW_BOARD_NAME string in the file.</summary>
    public BoardId? LegacyBoard { get; }

    /// <summary>A well-formed application image without the descriptor (8.6).</summary>
    public bool IsLegacy => Descriptor is null && HasAppDesc && Bytes.Length > 0 && Bytes[0] == 0xE9;
    public bool HasAppDesc { get; }
    public bool IsValid => Problems.Count == 0;
    public int Size => Bytes.Length;
    public string Version => Descriptor?.FwVersion ?? "";
    public BoardId Board => Descriptor?.Board ?? BoardId.Any;

    private FirmwareImage(byte[] bytes, string source)
    {
        Bytes = bytes;
        Source = source;
        Sha256 = SHA256.HashData(bytes);
        Crc32 = Protocol.Crc32.Compute(bytes);
        var problems = new List<string>();

        if (bytes.Length < FwDescriptor.Offset + FwDescriptor.Size || bytes[0] != 0xE9)
            problems.Add("not an ESP32 application image (byte 0 is not 0xE9)");
        HasAppDesc = bytes.Length >= 0x24 && BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(0x20)) == AppDescMagic;
        if (!HasAppDesc)
            problems.Add("no esp_app_desc_t at 0x20 (magic 0xABCD5432 missing)");
        else
            ImageId = ImageId.Read(bytes.AsSpan(ImageIdOffset));

        if (problems.Count == 0)
        {
            if (!FwDescriptor.HasMagic(bytes))
            {
                problems.Add("no NauticPinnace descriptor at 0x120 (legacy image, 1.2.0 or older)");
                LegacyBoard = FindLegacyBoard(bytes);
            }
            else
            {
                var d = FwDescriptor.Parse(bytes.AsSpan(FwDescriptor.Offset, FwDescriptor.Size));
                Descriptor = d;
                if (d.DescVersion < 1) problems.Add("descriptor version 0");
                if (d.DescSize < FwDescriptor.Size) problems.Add($"descriptor size {d.DescSize} < 64");
                if (!d.OtaGuard) problems.Add("OTAGUARD flag not set: the image could not confirm itself (10.2)");
                var panel = Boards.Panel(d.Board);
                if (panel is null) problems.Add($"unknown board id {(byte)d.Board}");
                else if (panel.Value.W != d.PanelWidth || panel.Value.H != d.PanelHeight)
                    problems.Add($"panel {d.PanelWidth}x{d.PanelHeight} does not match {Boards.DisplayName(d.Board)} ({panel.Value.W}x{panel.Value.H})");
                // Checked on the bytes: the display compares them with the OFFER's version
                // (8.2), and decoding would have turned a non-ASCII byte into '?'.
                if (!TextField.IsValidVersionField(bytes.AsSpan(FwDescriptor.Offset + 0x14, 16)))
                    problems.Add($"firmware version \"{d.FwVersion}\" is not 1-15 printable ASCII characters");
                if (d.N2kUpdateProto < 1) problems.Add("the image has no receiver for updates over NMEA 2000");
            }
        }
        Problems = problems;
    }

    public static FirmwareImage Load(string path) => new(File.ReadAllBytes(path), path);

    public static FirmwareImage FromBytes(byte[] bytes, string source = "memory") => new(bytes, source);

    /// <summary>
    /// The checks of 8.2 against one display: board id, panel, size, protocol.
    /// Returns the reasons the image must not be offered; empty means go.
    /// </summary>
    public IReadOnlyList<string> CheckAgainst(InfoMsg info)
    {
        var list = new List<string>(Problems);
        if (IsLegacy)
        {
            string board = LegacyBoard is { } b ? Boards.DisplayName(b) : "unknown board";
            list.Clear();
            list.Add($"legacy image for {board}: install over WiFi or USB");
            return list;
        }
        if (Descriptor is null) return list;
        if (Descriptor.Board != info.Board)
            list.Add($"firmware for {Boards.DisplayName(Descriptor.Board)}, but the display is a {Boards.DisplayName(info.Board)}");
        if (info.MaxFirmwareSize != 0 && (uint)Size > info.MaxFirmwareSize)
            list.Add($"image {Size} B is larger than the display's limit {info.MaxFirmwareSize} B");
        if (info.ProtoMax < UpdProto.VersionMin || info.ProtoMin > UpdProto.VersionMax || info.MaxChunkSize < UpdProto.ChunkSize)
            list.Add($"incompatible update protocol (display {info.ProtoMin}-{info.ProtoMax}, chunk {info.MaxChunkSize}; sender {UpdProto.VersionMin}-{UpdProto.VersionMax}, chunk {UpdProto.ChunkSize})");
        if ((info.Caps & Capabilities.Firmware) == 0)
            list.Add("the display does not accept firmware over NMEA 2000");
        return list;
    }

    /// <summary>Warnings that do not stop the update (13).</summary>
    public IReadOnlyList<string> WarningsAgainst(InfoMsg info)
    {
        var list = new List<string>();
        if (Descriptor is not null && Descriptor.N2kUpdateProto < info.ProtoMax)
            list.Add($"the new firmware speaks update protocol {Descriptor.N2kUpdateProto}, older than the running one ({info.ProtoMax}); the next update over NMEA 2000 might not work");
        return list;
    }

    private static BoardId? FindLegacyBoard(byte[] bytes)
    {
        BoardId? found = null;
        foreach (var b in Boards.Known)
        {
            byte[] name = Encoding.ASCII.GetBytes(Boards.BoardName(b)!);
            if (bytes.AsSpan().IndexOf(name) >= 0)
            {
                if (found is not null) return null;   // more than one: undecidable
                found = b;
            }
        }
        return found;
    }
}

/// <summary>A web UI file, <c>index.html.gz</c> (8.2: 1F 8B 08 at 0; size limits).</summary>
public sealed class WebUiImage
{
    public byte[] Bytes { get; }
    public string Source { get; }
    public byte[] Sha256 { get; }
    public uint Crc32 { get; }
    public IReadOnlyList<string> Problems { get; }

    /// <summary>First 8 bytes of the SHA-256 (3.0).</summary>
    public ImageId WebUiId => ImageId.Read(Sha256);
    public int Size => Bytes.Length;
    public bool IsValid => Problems.Count == 0;

    private WebUiImage(byte[] bytes, string source)
    {
        Bytes = bytes;
        Source = source;
        Sha256 = SHA256.HashData(bytes);
        Crc32 = Protocol.Crc32.Compute(bytes);
        var p = new List<string>();
        if (bytes.Length < 3 || bytes[0] != 0x1F || bytes[1] != 0x8B || bytes[2] != 0x08)
            p.Add("not a gzip file (1F 8B 08 missing)");
        Problems = p;
    }

    public static WebUiImage Load(string path) => new(File.ReadAllBytes(path), path);
    public static WebUiImage FromBytes(byte[] bytes, string source = "memory") => new(bytes, source);

    public IReadOnlyList<string> CheckAgainst(InfoMsg info)
    {
        var list = new List<string>(Problems);
        if (info.MaxWebUiSize != 0 && (uint)Size > info.MaxWebUiSize)
            list.Add($"web UI {Size} B is larger than the display's limit {info.MaxWebUiSize} B");
        if (info.ProtoMax < UpdProto.VersionMin || info.ProtoMin > UpdProto.VersionMax || info.MaxChunkSize < UpdProto.ChunkSize)
            list.Add($"incompatible update protocol (display {info.ProtoMin}-{info.ProtoMax}, chunk {info.MaxChunkSize})");
        if ((info.Caps & Capabilities.WebUi) == 0)
            list.Add("the display does not accept a web UI over NMEA 2000");
        return list;
    }
}
