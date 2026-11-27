namespace NauticPinnace.N2kUpdate.Can;

/// <summary>A 29-bit extended CAN frame with 0-8 data bytes. Every frame of this protocol has DLC 8.</summary>
public readonly struct CanFrame
{
    public uint Id { get; }
    public byte[] Data { get; }
    public int Length => Data.Length;

    public CanFrame(uint id, byte[] data)
    {
        if (data.Length > 8) throw new ArgumentException("CAN data is at most 8 bytes", nameof(data));
        Id = id & 0x1FFF_FFFF;
        Data = data;
    }

    public override string ToString() => $"{Id:X8} [{Length}] {Convert.ToHexString(Data)}";
}

/// <summary>
/// NMEA 2000 fields of a 29-bit identifier (J1939 layout). Ported from the
/// author's own earlier, unpublished slcan sender (same author, no third-party code).
///   bits 26-28 priority, bit 24 data page, bits 16-23 PDU format (PF),
///   bits 8-15 PDU specific (destination for PDU1, group extension for PDU2),
///   bits 0-7 source address. PF &lt; 240 is PDU1 (addressed), PF ≥ 240 is PDU2.
/// </summary>
public static class CanId
{
    public static uint Encode(int priority, int pgn, int source, int destination)
    {
        uint pf  = (uint)((pgn >> 8) & 0xFF);
        uint ps  = (uint)(pgn & 0xFF);
        uint dp  = (uint)((pgn >> 16) & 0x01);
        uint pri = (uint)(priority & 0x07);
        if (pf < 240)
            return (pri << 26) | (dp << 24) | (pf << 16) | ((uint)(destination & 0xFF) << 8) | (uint)(source & 0xFF);
        return (pri << 26) | (dp << 24) | (pf << 16) | (ps << 8) | (uint)(source & 0xFF);
    }

    public static (int Pgn, int Priority, int Source, int Destination) Decode(uint id)
    {
        int priority = (int)((id >> 26) & 0x07);
        int source   = (int)(id & 0xFF);
        uint dp      = (id >> 24) & 0x01;
        uint pf      = (id >> 16) & 0xFF;
        uint ps      = (id >> 8) & 0xFF;
        if (pf < 240)
            return ((int)((dp << 16) | (pf << 8)), priority, source, (int)ps);
        return ((int)((dp << 16) | (pf << 8) | ps), priority, source, 255);
    }

    public static bool IsPdu1(uint id) => ((id >> 16) & 0xFF) < 240;
}
