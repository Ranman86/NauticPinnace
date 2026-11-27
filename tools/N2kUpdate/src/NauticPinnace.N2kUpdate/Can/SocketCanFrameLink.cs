using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;

namespace NauticPinnace.N2kUpdate.Can;

/// <summary>
/// SocketCAN link for Linux (12.2; NauticPi, if it drives the bus through can0
/// instead of slcan). Raw CAN socket, one 16-byte <c>struct can_frame</c> per frame.
/// 6.4: the interface's txqueuelen MUST be at least 64 (<c>ip link set can0 txqueuelen 128</c>);
/// ENOBUFS from a write is handled like a BELL in stop-and-wait: wait 2 ms and send the
/// same frame again, at most 50 times. Frames of one burst are written under one lock,
/// so no other message of this process can come between them.
/// Not exercised by the test suite (no CAN interface on the build machine).
/// </summary>
public sealed class SocketCanFrameLink : ICanFrameLink
{
    private const uint CanEffFlag = 0x8000_0000, CanRtrFlag = 0x4000_0000, CanErrFlag = 0x2000_0000;
    private const int CanRaw = 1;

    private readonly string _iface;
    private readonly IClock _clock;
    private readonly object _tx = new();
    private Socket? _sock;
    private Thread? _rx;
    private volatile bool _open;

    public SocketCanFrameLink(string interfaceName = "can0", IClock? clock = null)
    {
        _iface = interfaceName;
        _clock = clock ?? SystemClock.Instance;
    }

    public bool IsOpen => _open;
    public LinkTxMode TxMode { get; set; }
    public event FrameReceivedHandler? FrameReceived;
    public event LinkEventHandler? LinkEvent;

    /// <summary>The interface's tx_queue_len, or -1 if unknown (6.4 wants at least 64).</summary>
    public int TxQueueLength
    {
        get
        {
            try { return int.Parse(File.ReadAllText($"/sys/class/net/{_iface}/tx_queue_len").Trim()); }
            catch (Exception ex) when (ex is IOException or FormatException or UnauthorizedAccessException) { return -1; }
        }
    }

    public void Open()
    {
        if (!OperatingSystem.IsLinux()) throw new PlatformNotSupportedException("SocketCAN needs Linux");
        Close();
        // 6.4: a queue shorter than one DATA burst plus control messages drops frames
        // of every message (MUST be at least 64). An unreadable length is let through.
        int q = TxQueueLength;
        if (q is >= 0 and < 64)
            throw new IOException($"{_iface}: txqueuelen is {q}, at least 64 are needed (ip link set {_iface} txqueuelen 128)");
        int index;
        try { index = int.Parse(File.ReadAllText($"/sys/class/net/{_iface}/ifindex").Trim()); }
        catch (Exception ex) when (ex is IOException or FormatException) { throw new IOException($"no CAN interface {_iface}", ex); }
        var s = new Socket(AddressFamily.ControllerAreaNetwork, SocketType.Raw, (ProtocolType)CanRaw);
        s.Bind(new CanEndPoint(index));
        _sock = s;
        _open = true;
        _rx = new Thread(RxLoop) { IsBackground = true, Name = "socketcan-rx" };
        _rx.Start();
    }

    public void Close()
    {
        bool was = _open;
        _open = false;
        try { _sock?.Dispose(); } catch { }
        _sock = null;
        if (was) LinkEvent?.Invoke(LinkEventKind.Closed, "closed", _clock.NowUs);
    }

    public void Dispose() => Close();

    public void SendBurst(IReadOnlyList<CanFrame> frames)
    {
        var s = _sock ?? throw new IOException("CAN link is not open");
        var buf = new byte[16];
        lock (_tx)
        {
            foreach (var f in frames)
            {
                buf.AsSpan().Clear();
                BinaryPrimitives.WriteUInt32LittleEndian(buf, (f.Id & 0x1FFF_FFFF) | CanEffFlag);
                buf[4] = (byte)f.Length;
                f.Data.CopyTo(buf, 8);
                for (int attempt = 0; ; attempt++)
                {
                    try { s.Send(buf); break; }
                    catch (SocketException ex) when (ex.SocketErrorCode == SocketError.NoBufferSpaceAvailable && attempt < 50)
                    {
                        // ENOBUFS: the queue is full. Like a BELL in stop-and-wait (6.4).
                        LinkEvent?.Invoke(LinkEventKind.Bell, "ENOBUFS", _clock.NowUs);
                        long end = Stopwatch.GetTimestamp() + Stopwatch.Frequency / 500;
                        while (Stopwatch.GetTimestamp() < end) Thread.SpinWait(20);
                    }
                    catch (Exception ex) when (ex is SocketException or ObjectDisposedException)
                    {
                        LinkEvent?.Invoke(LinkEventKind.Error, ex.Message, _clock.NowUs);
                        throw new IOException("CAN write failed: " + ex.Message, ex);
                    }
                }
            }
        }
    }

    private void RxLoop()
    {
        var buf = new byte[16];
        var s = _sock;
        string? why = null;
        while (_open && s is not null)
        {
            int n;
            try { n = s.Receive(buf); }
            catch (Exception ex) when (ex is SocketException or ObjectDisposedException) { why = ex.Message; break; }
            if (n < 16) continue;
            uint id = BinaryPrimitives.ReadUInt32LittleEndian(buf);
            if ((id & CanEffFlag) == 0 || (id & (CanRtrFlag | CanErrFlag)) != 0) continue;
            int len = Math.Min(8, (int)buf[4]);
            FrameReceived?.Invoke(new CanFrame(id & 0x1FFF_FFFF, buf.AsSpan(8, len).ToArray()), _clock.NowUs);
        }
        if (_open)
        {
            _open = false;
            LinkEvent?.Invoke(LinkEventKind.Closed, why ?? "receive loop ended", _clock.NowUs);
        }
    }

    /// <summary><c>struct sockaddr_can</c>: family, interface index, unused address union.</summary>
    private sealed class CanEndPoint(int ifIndex) : EndPoint
    {
        public override AddressFamily AddressFamily => AddressFamily.ControllerAreaNetwork;

        public override SocketAddress Serialize()
        {
            // The family is written by SocketAddress itself; the index follows at offset 4.
            var sa = new SocketAddress(AddressFamily.ControllerAreaNetwork, 24);
            BinaryPrimitives.WriteInt32LittleEndian(sa.Buffer.Span[4..], ifIndex);
            return sa;
        }

        public override EndPoint Create(SocketAddress socketAddress) => this;
    }
}
