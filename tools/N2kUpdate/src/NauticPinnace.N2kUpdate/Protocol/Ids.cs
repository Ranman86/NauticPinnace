using System.Text;

namespace NauticPinnace.N2kUpdate.Protocol;

/// <summary>
/// A display's unique id: the ESP32 base MAC, 6 bytes. Transmission order is
/// display order (3.2), so <c>02:00:00:00:00:01</c> goes out as <c>02 00 00 00 00 01</c>.
/// Displays are identified by this id, never by their address (5.2).
/// </summary>
public readonly record struct UniqueId(ulong Value)
{
    /// <summary>The filter value "any display" (FF×6) of QUERY.</summary>
    public static readonly UniqueId Any = new(0xFFFF_FFFF_FFFF);

    public bool IsAny => Value == Any.Value;

    public static UniqueId Read(ReadOnlySpan<byte> s)
    {
        ulong v = 0;
        for (int i = 0; i < 6; i++) v = (v << 8) | s[i];
        return new UniqueId(v);
    }

    public void Write(Span<byte> d)
    {
        for (int i = 0; i < 6; i++) d[i] = (byte)(Value >> (8 * (5 - i)));
    }

    public byte[] ToBytes() { var b = new byte[6]; Write(b); return b; }

    /// <summary>The last three bytes, as the identify screen shows them (7.7).</summary>
    public string ShortText => $"{(byte)(Value >> 16):X2}:{(byte)(Value >> 8):X2}:{(byte)Value:X2}";

    /// <summary>The last six hex digits, as the updater's table shows them (12.7).</summary>
    public string LastSixHex => (Value & 0xFFFFFF).ToString("X6");

    public override string ToString()
    {
        var sb = new StringBuilder(17);
        for (int i = 0; i < 6; i++)
        {
            if (i > 0) sb.Append(':');
            sb.Append(((byte)(Value >> (8 * (5 - i)))).ToString("X2"));
        }
        return sb.ToString();
    }

    /// <summary>Parses "02:00:00:00:00:01", "02-00-…" or "020000000001".</summary>
    public static bool TryParse(string? text, out UniqueId id)
    {
        id = default;
        if (string.IsNullOrWhiteSpace(text)) return false;
        string hex = new(text.Where(Uri.IsHexDigit).ToArray());
        if (hex.Length != 12) return false;
        id = new UniqueId(Convert.ToUInt64(hex, 16));
        return true;
    }
}

/// <summary>
/// An 8-byte id: the image id of a firmware (first 8 bytes of
/// <c>esp_app_desc_t.app_elf_sha256</c>, file offset 0xB0, 8.3) or the web UI id
/// (first 8 bytes of the SHA-256 of index.html.gz, 3.0). Stored in file order;
/// all zero means "none".
/// </summary>
public readonly record struct ImageId(ulong Value)
{
    public static readonly ImageId None = new(0);

    public bool IsNone => Value == 0;

    public static ImageId Read(ReadOnlySpan<byte> s)
    {
        ulong v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | s[i];
        return new ImageId(v);
    }

    public void Write(Span<byte> d)
    {
        for (int i = 0; i < 8; i++) d[i] = (byte)(Value >> (8 * (7 - i)));
    }

    public override string ToString()
    {
        Span<byte> b = stackalloc byte[8];
        Write(b);
        return Convert.ToHexString(b);
    }
}

/// <summary>Fixed-size text fields of section 3.0.</summary>
public static class TextField
{
    /// <summary>
    /// A version field <c>str[16]</c>: 1-15 printable ASCII characters (0x20-0x7E),
    /// NUL-terminated and NUL-padded.
    /// </summary>
    public static bool IsValidVersion(string? v) =>
        !string.IsNullOrEmpty(v) && v.Length <= 15 && v.All(c => c >= 0x20 && c <= 0x7E);

    /// <summary>
    /// The same rule on the raw 16 bytes of a field (the descriptor's fw_version, 8.1):
    /// a decoder would turn a byte outside 0x20-0x7E into '?' and pass it.
    /// </summary>
    public static bool IsValidVersionField(ReadOnlySpan<byte> f)
    {
        if (f.Length < 16) return false;
        int n = f[..16].IndexOf((byte)0);
        if (n < 1 || n > 15) return false;
        for (int i = 0; i < n; i++) if (f[i] < 0x20 || f[i] > 0x7E) return false;
        for (int i = n; i < 16; i++) if (f[i] != 0) return false;
        return true;
    }

    public static void WriteVersion(Span<byte> d, string? v)
    {
        d[..16].Clear();
        if (string.IsNullOrEmpty(v)) return;
        for (int i = 0; i < Math.Min(15, v.Length); i++)
        {
            char c = v[i];
            d[i] = c >= 0x20 && c <= 0x7E ? (byte)c : (byte)'?';
        }
    }

    /// <summary>UTF-8, at most <paramref name="size"/> bytes, cut at a character boundary, NUL-padded.</summary>
    public static void WriteUtf8(Span<byte> d, string? s, int size = 16)
    {
        d[..size].Clear();
        if (string.IsNullOrEmpty(s)) return;
        byte[] all = Encoding.UTF8.GetBytes(s);
        int n = Math.Min(size, all.Length);
        // Back off to a character boundary: a UTF-8 continuation byte is 10xxxxxx.
        while (n > 0 && n < all.Length && (all[n] & 0xC0) == 0x80) n--;
        all.AsSpan(0, n).CopyTo(d);
    }

    /// <summary>Reads a NUL-padded field; the text ends at the first NUL or at the field end.</summary>
    public static string Read(ReadOnlySpan<byte> s)
    {
        int n = s.IndexOf((byte)0);
        if (n < 0) n = s.Length;
        return Encoding.UTF8.GetString(s[..n]);
    }

    /// <summary>UTF-8 length check used for sender names and labels (at most 16 bytes).</summary>
    public static int Utf8Length(string? s) => s is null ? 0 : Encoding.UTF8.GetByteCount(s);
}
