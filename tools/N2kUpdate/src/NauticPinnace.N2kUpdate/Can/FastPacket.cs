namespace NauticPinnace.N2kUpdate.Can;

/// <summary>
/// NMEA 2000 fast-packet framing (2.3). Unlike encoders that send payloads of 8 bytes
/// or less unframed (the author's earlier, unpublished slcan sender among them),
/// this encoder ALWAYS frames: the receiving library treats every 126720 frame as
/// fast packet (2.3 rule 1).
///   frame 0: [seq&lt;&lt;5 | 0] [total length] [payload 0..5]
///   frame k: [seq&lt;&lt;5 | k] [next 7 payload bytes]
/// Unused bytes in the last frame are 0xFF; DLC is always 8.
/// </summary>
public static class FastPacket
{
    public const int MaxPayload = 223;

    public static int FrameCount(int payloadLength) =>
        payloadLength <= 6 ? 1 : 1 + (payloadLength - 6 + 6) / 7;

    public static List<CanFrame> Encode(uint canId, ReadOnlySpan<byte> payload, int seq)
    {
        var list = new List<CanFrame>(FrameCount(payload.Length));
        Encode(canId, payload, seq, list);
        return list;
    }

    public static void Encode(uint canId, ReadOnlySpan<byte> payload, int seq, List<CanFrame> output)
    {
        if (payload.Length < 1 || payload.Length > MaxPayload)
            throw new ArgumentOutOfRangeException(nameof(payload), "fast-packet payload must be 1..223 bytes");
        int s = (seq & 0x07) << 5;
        var f0 = new byte[8];
        f0[0] = (byte)s;
        f0[1] = (byte)payload.Length;
        int pos = 0;
        for (int i = 2; i < 8; i++) f0[i] = pos < payload.Length ? payload[pos++] : (byte)0xFF;
        output.Add(new CanFrame(canId, f0));
        int k = 1;
        while (pos < payload.Length)
        {
            var f = new byte[8];
            f[0] = (byte)(s | (k & 0x1F));
            for (int i = 1; i < 8; i++) f[i] = pos < payload.Length ? payload[pos++] : (byte)0xFF;
            output.Add(new CanFrame(canId, f));
            k++;
        }
    }
}

/// <summary>
/// The 3-bit fast-packet sequence counter per (source address, PGN) (2.3 rule 4).
/// It goes up once per message.
/// </summary>
public sealed class FastPacketSequencer
{
    private readonly Dictionary<(int Src, int Pgn), int> _next = new();

    public int Peek(int source, int pgn) => _next.TryGetValue((source, pgn), out int s) ? s : 0;

    public int Take(int source, int pgn)
    {
        int s = Peek(source, pgn);
        _next[(source, pgn)] = (s + 1) & 0x07;
        return s;
    }
}

/// <summary>
/// Fast-packet reassembly for the sender (12.2). Messages are keyed by
/// (source, destination, PGN) and must arrive in order; a missing or out-of-order
/// frame drops the whole message, as on the receiving library (2.3 rule 6). Gaps of
/// up to 2 s between frames of one message are accepted, because the 4-inch's TX
/// stalls during flash operations (2.4). A new frame 0 for the same key restarts.
/// The PDU1 destination filter is applied by the caller BEFORE frames get here.
/// </summary>
public sealed class FastPacketAssembler
{
    private sealed class Slot
    {
        public int Seq;
        public int Next;
        public int Length;
        public int Filled;
        public long LastUs;
        public readonly byte[] Buf = new byte[FastPacket.MaxPayload];
    }

    private readonly Dictionary<(int Src, int Dst, int Pgn), Slot> _slots = new();

    /// <summary>Largest accepted gap between two frames of one message.</summary>
    public long GapToleranceUs { get; set; } = 2_000_000;

    /// <summary>Messages dropped because of a missing or out-of-order frame or a gap.</summary>
    public long Dropped { get; private set; }

    /// <summary>
    /// Adds one frame of a fast-packet PGN. Returns the payload once the message is
    /// complete, otherwise null.
    /// </summary>
    public byte[]? Add(int source, int destination, int pgn, ReadOnlySpan<byte> data, long nowUs)
    {
        if (data.Length < 2) return null;
        var key = (source, destination, pgn);
        int seq = data[0] >> 5;
        int index = data[0] & 0x1F;

        if (index == 0)
        {
            int len = data[1];
            if (len == 0 || len > FastPacket.MaxPayload) { _slots.Remove(key); return null; }
            if (_slots.Remove(key)) Dropped++;   // an unfinished message is replaced
            int take = Math.Min(len, Math.Min(6, data.Length - 2));
            if (take >= len)
                return data.Slice(2, len).ToArray();
            var slot = new Slot { Seq = seq, Next = 1, Length = len, Filled = take, LastUs = nowUs };
            data.Slice(2, take).CopyTo(slot.Buf);
            _slots[key] = slot;
            return null;
        }

        if (!_slots.TryGetValue(key, out var s)) return null;
        if (s.Seq != seq || s.Next != index || nowUs - s.LastUs > GapToleranceUs)
        {
            _slots.Remove(key);
            Dropped++;
            return null;
        }
        int n = Math.Min(7, Math.Min(s.Length - s.Filled, data.Length - 1));
        data.Slice(1, n).CopyTo(s.Buf.AsSpan(s.Filled));
        s.Filled += n;
        s.Next++;
        s.LastUs = nowUs;
        if (s.Filled < s.Length) return null;
        _slots.Remove(key);
        return s.Buf.AsSpan(0, s.Length).ToArray();
    }

    /// <summary>Forgets messages whose last frame is older than the gap tolerance.</summary>
    public void Expire(long nowUs)
    {
        if (_slots.Count == 0) return;
        List<(int, int, int)>? old = null;
        foreach (var kv in _slots)
            if (nowUs - kv.Value.LastUs > GapToleranceUs) (old ??= new()).Add(kv.Key);
        if (old is null) return;
        foreach (var k in old) { _slots.Remove(k); Dropped++; }
    }

    public void Clear() => _slots.Clear();
}
