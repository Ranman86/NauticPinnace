namespace NauticPinnace.N2kUpdate.Can;

/// <summary>Events of a CAN link besides received frames (12.2).</summary>
public enum LinkEventKind
{
    /// <summary>The link was closed (adapter unplugged, stream ended, Close()).</summary>
    Closed,
    /// <summary>A transport error (USB error, write timeout).</summary>
    Error,
    /// <summary>The adapter answered BELL (0x07): ring full, frame dropped (6.4).</summary>
    Bell,
}

/// <summary>How a link hands frames to the adapter (6.4).</summary>
public enum LinkTxMode
{
    /// <summary>All frames of one burst in one write.</summary>
    Normal,
    /// <summary>
    /// One frame per write, wait for its reply (at most 20 ms); on BELL wait 2 ms and
    /// send the same frame again, at most 50 times, then report a link error. For
    /// adapters whose buffer is smaller than one message.
    /// </summary>
    StopAndWait,
}

/// <summary>Raised for every received frame; the timestamp is µs on the engine's <see cref="IClock"/>.</summary>
public delegate void FrameReceivedHandler(CanFrame frame, long timestampUs);

/// <summary>Raised for link events; the timestamp is µs on the engine's <see cref="IClock"/>.</summary>
public delegate void LinkEventHandler(LinkEventKind kind, string? detail, long timestampUs);

/// <summary>
/// A CAN adapter as the sender library sees it (12.2). Implementations:
/// <see cref="SlcanFrameLink"/> (CANable over a serial port, or slcan text over any
/// stream such as TCP or a child process), <see cref="DryFrameLink"/> (no hardware,
/// records every frame), and the virtual bus of the test assembly. NauticPi adds an
/// adapter over its shared CANable port gate.
/// </summary>
public interface ICanFrameLink : IDisposable
{
    bool IsOpen { get; }

    /// <summary>Opens the adapter (C, S5, O for slcan). Throws on failure.</summary>
    void Open();

    void Close();

    /// <summary>
    /// Sends the frames contiguously and in order. No frame of another message of
    /// the same source and PGN may come between them (2.3 rule 5), so a link must
    /// write a burst atomically with respect to other bursts.
    /// </summary>
    void SendBurst(IReadOnlyList<CanFrame> frames);

    LinkTxMode TxMode { get; set; }

    event FrameReceivedHandler? FrameReceived;
    event LinkEventHandler? LinkEvent;
}
