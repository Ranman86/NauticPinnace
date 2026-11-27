namespace NauticPinnace.N2kUpdate.Protocol;

/// <summary>
/// CRC-32/ISO-HDLC, the same as zlib <c>crc32()</c> and <c>esp_rom_crc32_le</c>:
/// polynomial 0x04C11DB7 reflected, init 0xFFFFFFFF, final XOR 0xFFFFFFFF (8.5).
/// <c>Update(0, data)</c> is the CRC of <c>data</c>; chaining works like
/// zlib: <c>Update(Update(0, a), b) == Update(0, a + b)</c>.
/// </summary>
public static class Crc32
{
    private static readonly uint[] Table = BuildTable();

    private static uint[] BuildTable()
    {
        var t = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }

    public static uint Update(uint crc, ReadOnlySpan<byte> data)
    {
        uint c = ~crc;
        foreach (byte b in data) c = Table[(c ^ b) & 0xFF] ^ (c >> 8);
        return ~c;
    }

    public static uint Compute(ReadOnlySpan<byte> data) => Update(0, data);
}

/// <summary>
/// CRC over a growing prefix of one stream, as the sender needs it for every ACK
/// (5.4, sender step 1). Extends the last value instead of starting over.
/// </summary>
public sealed class PrefixCrc
{
    private readonly ReadOnlyMemory<byte> _stream;
    private long _len;
    private uint _crc;

    public PrefixCrc(ReadOnlyMemory<byte> stream) => _stream = stream;

    /// <summary>CRC-32 over stream bytes [0, length); 0 for length 0 (3.6).</summary>
    public uint Get(long length)
    {
        if (length < 0 || length > _stream.Length) throw new ArgumentOutOfRangeException(nameof(length));
        if (length < _len) { _len = 0; _crc = 0; }
        if (length > _len)
        {
            _crc = Crc32.Update(_crc, _stream.Span[(int)_len..(int)length]);
            _len = length;
        }
        return _crc;
    }
}
