using System.IO.Compression;
using System.Security.Cryptography;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Images;

/// <summary>
/// Everything one session sends: the decoded image (what the display writes), the
/// stream (what DATA carries), and their checksums, all computed once (12.4).
/// </summary>
public sealed class TransferImage
{
    public UpdateTarget Target { get; }
    /// <summary>Firmware: from the descriptor. Web UI: 0 = any board.</summary>
    public BoardId Board { get; }
    public byte[] Image { get; }
    public byte[] Sha256 { get; }
    public StreamEncoding Encoding { get; }
    public byte[] Stream { get; }
    public uint StreamCrc32 { get; }
    /// <summary>Firmware: descriptor fw_version. Web UI: the firmware version it belongs to.</summary>
    public string Version { get; }
    /// <summary>Firmware: image id (8.3). Web UI: web UI id (3.0).</summary>
    public ImageId Id { get; }

    public uint ImageSize => (uint)Image.Length;
    public uint StreamSize => (uint)Stream.Length;
    public int ChunkCount => (Stream.Length + UpdProto.ChunkSize - 1) / UpdProto.ChunkSize;

    private TransferImage(UpdateTarget target, BoardId board, byte[] image, byte[] sha, StreamEncoding enc,
                          byte[] stream, string version, ImageId id)
    {
        Target = target; Board = board; Image = image; Sha256 = sha; Encoding = enc;
        Stream = stream; StreamCrc32 = Crc32.Compute(stream); Version = version; Id = id;
        if (stream.Length == 0 || stream.Length > UpdProto.MaxStreamSize)
            throw new InvalidDataException($"stream size {stream.Length} outside 1..{UpdProto.MaxStreamSize}");
    }

    /// <summary>
    /// A firmware transfer. With <paramref name="deflate"/> the stream is the raw
    /// deflate of the image (9.3), taken from the cache when possible; if deflate
    /// would not stay within the bound of 3.3, the image is sent raw.
    /// </summary>
    public static TransferImage ForFirmware(FirmwareImage fw, bool deflate, DeflateCache? cache = null)
    {
        if (fw.Descriptor is null) throw new InvalidOperationException("legacy image without descriptor");
        byte[] stream = fw.Bytes;
        var enc = StreamEncoding.Raw;
        if (deflate)
        {
            byte[] d = (cache ?? new DeflateCache(null)).GetOrCreate(fw.Bytes, fw.Sha256);
            if (d.Length < fw.Bytes.Length && d.Length <= UpdProto.StreamBound(fw.Bytes.Length))
            {
                stream = d;
                enc = StreamEncoding.Deflate;
            }
        }
        return new TransferImage(UpdateTarget.Firmware, fw.Board, fw.Bytes, fw.Sha256, enc, stream, fw.Version, fw.ImageId);
    }

    /// <summary>A web UI transfer; always raw (4.3: the file is already gzip).</summary>
    public static TransferImage ForWebUi(WebUiImage web, string version) =>
        new(UpdateTarget.WebUi, BoardId.Any, web.Bytes, web.Sha256, StreamEncoding.Raw, web.Bytes, version, web.WebUiId);
}

/// <summary>
/// The deflate stream of an image, produced once and kept in memory and on disk,
/// keyed by (image SHA-256, encoder id) (12.4). A restarted sender then RESUMEs with
/// a byte-identical stream (5.3.4); without the cache it would have to offer anew.
///
/// A cache file starts with a header - magic, stream length, SHA-256 of the stream -
/// so a file that was cut short or got bytes appended is rebuilt. Inflating alone
/// would not notice bytes after the final deflate block, which the display rejects
/// only after the whole transfer (SIZE_MISMATCH, 5.5 step 2).
/// </summary>
public sealed class DeflateCache
{
    /// <summary>The encoder id: .NET 10 DeflateStream at CompressionLevel.SmallestSize (9.3).</summary>
    public const string EncoderId = "dotnet10-deflate-smallest";

    private static ReadOnlySpan<byte> Magic => "NPDEFL01"u8;
    private const int HeaderSize = 8 + 4 + 32;

    private readonly string? _dir;
    private readonly Dictionary<string, byte[]> _mem = new();
    private readonly object _lock = new();

    /// <param name="directory">e.g. %LOCALAPPDATA%\NauticPinnaceUpdater\cache, or NauticPi's data folder; null = memory only.</param>
    public DeflateCache(string? directory) => _dir = directory;

    /// <summary>True if the last <see cref="GetOrCreate"/> came from disk.</summary>
    public bool LastWasFromDisk { get; private set; }

    public string? PathFor(byte[] sha256) =>
        _dir is null ? null : Path.Combine(_dir, $"{Convert.ToHexString(sha256).ToLowerInvariant()}-{EncoderId}.deflate");

    public byte[] GetOrCreate(byte[] image, byte[] sha256)
    {
        string key = Convert.ToHexString(sha256);
        lock (_lock)
        {
            LastWasFromDisk = false;
            if (_mem.TryGetValue(key, out var hit)) return hit;
            string? path = PathFor(sha256);
            if (path is not null && File.Exists(path))
            {
                try
                {
                    byte[] file = File.ReadAllBytes(path);
                    // Never trust the disk blindly: exactly the stream the header
                    // describes, and it must inflate to exactly this image.
                    if (Unwrap(file) is { } cached && InflatesTo(cached, image.Length, sha256))
                    {
                        _mem[key] = cached;
                        LastWasFromDisk = true;
                        return cached;
                    }
                }
                catch (IOException) { /* unreadable cache entry: rebuild */ }
                catch (InvalidDataException) { }
            }
            byte[] d = Deflate(image);
            _mem[key] = d;
            if (path is not null)
            {
                try
                {
                    Directory.CreateDirectory(_dir!);
                    string tmp = path + ".tmp";
                    File.WriteAllBytes(tmp, Wrap(d));
                    File.Move(tmp, path, overwrite: true);
                }
                catch (IOException) { /* the cache is a convenience */ }
                catch (UnauthorizedAccessException) { }
            }
            return d;
        }
    }

    private static byte[] Wrap(byte[] stream)
    {
        var f = new byte[HeaderSize + stream.Length];
        Magic.CopyTo(f);
        System.Buffers.Binary.BinaryPrimitives.WriteInt32LittleEndian(f.AsSpan(8), stream.Length);
        SHA256.HashData(stream).CopyTo(f, 12);
        stream.CopyTo(f, HeaderSize);
        return f;
    }

    // The stream of a cache file, or null if the header does not describe exactly it.
    private static byte[]? Unwrap(byte[] f)
    {
        if (f.Length < HeaderSize || !f.AsSpan(0, 8).SequenceEqual(Magic)) return null;
        int len = System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(f.AsSpan(8));
        if (len != f.Length - HeaderSize) return null;
        byte[] s = f.AsSpan(HeaderSize).ToArray();
        return SHA256.HashData(s).AsSpan().SequenceEqual(f.AsSpan(12, 32)) ? s : null;
    }

    /// <summary>Raw deflate (RFC 1951, no zlib or gzip wrapper), as the display's tinfl expects (4.3).</summary>
    public static byte[] Deflate(byte[] image)
    {
        using var ms = new MemoryStream(image.Length / 2 + 1024);
        using (var ds = new DeflateStream(ms, CompressionLevel.SmallestSize, leaveOpen: true))
            ds.Write(image, 0, image.Length);
        return ms.ToArray();
    }

    public static byte[] Inflate(byte[] stream, int maxLength)
    {
        using var input = new MemoryStream(stream);
        using var ds = new DeflateStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream();
        var buf = new byte[65536];
        int n;
        while ((n = ds.Read(buf, 0, buf.Length)) > 0)
        {
            output.Write(buf, 0, n);
            if (output.Length > maxLength) throw new InvalidDataException("inflates beyond the image size");
        }
        return output.ToArray();
    }

    private static bool InflatesTo(byte[] stream, int length, byte[] sha256)
    {
        byte[] plain = Inflate(stream, length);
        return plain.Length == length && SHA256.HashData(plain).AsSpan().SequenceEqual(sha256);
    }
}
