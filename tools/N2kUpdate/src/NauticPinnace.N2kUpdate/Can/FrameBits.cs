namespace NauticPinnace.N2kUpdate.Can;

/// <summary>
/// Exact on-wire length of a 29-bit CAN frame (6.1, 6.2): the bits SOF..CRC with
/// the real CRC-15 (polynomial 0x4599), plus the stuff bits inserted after every
/// run of five equal bits, plus the 13-bit unstuffed tail (CRC delimiter, ACK slot
/// and delimiter, 7 EOF bits, 3 intermission bits). Reference: frame_bits() in
/// vectors.py; the test values are in Appendix A.
/// </summary>
public static class FrameBits
{
    /// <summary>Worst case for a frame with 8 data bytes (6.1).</summary>
    public const int Conservative = 160;

    public static int Exact(CanFrame f) => Exact(f.Id, f.Data);

    public static int Exact(uint id, ReadOnlySpan<byte> data)
    {
        // At most 1 + 11 + 2 + 18 + 3 + 4 + 64 + 15 = 118 stuffable bits.
        Span<byte> bits = stackalloc byte[128];
        int n = 0;
        bits[n++] = 0;                                                  // SOF
        for (int i = 0; i < 11; i++) bits[n++] = (byte)((id >> (28 - i)) & 1);   // ID A
        bits[n++] = 1;                                                  // SRR
        bits[n++] = 1;                                                  // IDE
        for (int i = 0; i < 18; i++) bits[n++] = (byte)((id >> (17 - i)) & 1);   // ID B
        bits[n++] = 0; bits[n++] = 0; bits[n++] = 0;                    // RTR, r1, r0
        int dlc = data.Length;
        for (int i = 0; i < 4; i++) bits[n++] = (byte)((dlc >> (3 - i)) & 1);
        foreach (byte b in data)
            for (int i = 0; i < 8; i++) bits[n++] = (byte)((b >> (7 - i)) & 1);

        int crc = 0;
        for (int i = 0; i < n; i++)
        {
            int next = bits[i] ^ ((crc >> 14) & 1);
            crc = (crc << 1) & 0x7FFF;
            if (next != 0) crc ^= 0x4599;
        }
        for (int i = 0; i < 15; i++) bits[n++] = (byte)((crc >> (14 - i)) & 1);

        int stuffed = 0, runVal = -1, runLen = 0;
        for (int i = 0; i < n; i++)
        {
            int b = bits[i];
            if (b == runVal) runLen++;
            else { runVal = b; runLen = 1; }
            if (runLen == 5)
            {
                // The stuff bit has the opposite value and starts a new run.
                stuffed++;
                runVal = 1 - b;
                runLen = 1;
            }
        }
        return n + stuffed + 13;
    }

    /// <summary>The cost of a frame for the token bucket: exact, or 160 bits when conservative (6.2).</summary>
    public static int Cost(CanFrame f, bool conservative) => conservative ? Conservative : Exact(f);
}
