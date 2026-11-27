using System.Diagnostics;
using System.IO.Ports;
using System.Net.Sockets;
using System.Text;

namespace NauticPinnace.N2kUpdate.Can;

/// <summary>
/// slcan (Lawicel ASCII) link, for a CANable with slcan firmware at 250 kbit/s, or
/// for slcan text over any byte stream (TCP, a child process's stdin/stdout) as the
/// cross test uses it (14.3.3). The serial part is ported from the author's own
/// earlier, unpublished slcan sender (same author, no third-party code); the burst
/// write, the reply accounting and stop-and-wait are new (6.4).
///
/// Commands, each terminated by CR: <c>C</c> close, <c>S5</c> 250 kbit/s, <c>O</c> open,
/// <c>T&lt;id8&gt;&lt;dlc&gt;&lt;data&gt;</c> send a 29-bit frame (27 bytes with 8 data bytes).
/// The adapter answers CR ("queued", not "sent") or BELL 0x07 ("ring full, frame
/// dropped"). Received frames arrive as the same T lines.
/// </summary>
public sealed class SlcanFrameLink : ICanFrameLink
{
    private readonly Func<Stream>? _streamFactory;
    private readonly Func<string?>? _portResolver;
    private readonly IClock _clock;
    private readonly object _txLock = new();
    private readonly AutoResetEvent _replySignal = new(false);

    private SerialPort? _port;
    private Stream? _stream;
    private Thread? _rxThread;
    private volatile bool _rxRun;
    private volatile bool _open;

    private long _acks, _bells;

    /// <summary>A human-readable description of the endpoint (port name, host:port, command).</summary>
    public string Description { get; private set; }

    /// <summary>
    /// DTR and RTS on open. A CANable (STM32 USB CDC) is not reset by them, and
    /// common slcan tools assert both. An ESP32-S3 on the other hand drops into its
    /// bootloader, so a host that opens a port it has NOT identified as a CANable
    /// should set this to false.
    /// </summary>
    public bool AssertDtrRts { get; set; } = true;

    public LinkTxMode TxMode { get; set; } = LinkTxMode.Normal;

    public long FramesSent { get; private set; }
    public long FramesReceived { get; private set; }
    public long Bells => Interlocked.Read(ref _bells);
    public long Replies => Interlocked.Read(ref _acks);

    public bool IsOpen => _open;

    public event FrameReceivedHandler? FrameReceived;
    public event LinkEventHandler? LinkEvent;

    /// <summary>A CANable on a fixed serial port.</summary>
    public SlcanFrameLink(string portName, IClock? clock = null)
        : this(() => portName, clock) { }

    /// <summary>
    /// A CANable whose port is looked up at every open, because it wanders when other
    /// USB serial devices are plugged in (5.8: reopen after unplugging).
    /// </summary>
    public SlcanFrameLink(Func<string?> portResolver, IClock? clock = null)
    {
        _portResolver = portResolver;
        _clock = clock ?? SystemClock.Instance;
        Description = "slcan";
    }

    /// <summary>slcan text over an arbitrary stream; the factory is called at every open.</summary>
    public SlcanFrameLink(Func<Stream> streamFactory, string description, IClock? clock = null)
    {
        _streamFactory = streamFactory;
        _clock = clock ?? SystemClock.Instance;
        Description = description;
    }

    /// <summary>slcan text over TCP, for the live cross test against n2kupd_hostsim (14.3.3).</summary>
    public static SlcanFrameLink ForTcp(string host, int port, IClock? clock = null) =>
        new(() =>
        {
            var c = new TcpClient { NoDelay = true };
            c.Connect(host, port);
            return new OwningStream(c.GetStream(), c);
        }, $"tcp:{host}:{port}", clock);

    /// <summary>
    /// slcan text over TCP, with this side as the server: open() waits for one client
    /// (for a bridge that prefers to connect, e.g. a receiver under a debugger).
    /// </summary>
    public static SlcanFrameLink ForTcpListen(int port, IClock? clock = null) =>
        new(() =>
        {
            var l = new TcpListener(System.Net.IPAddress.Loopback, port);
            l.Start();
            try
            {
                var c = l.AcceptTcpClient();
                c.NoDelay = true;
                return new OwningStream(c.GetStream(), c);
            }
            finally { l.Stop(); }
        }, $"tcp-listen:{port}", clock);

    /// <summary>
    /// slcan text over a child process's stdin (our TX) and stdout (our RX). The
    /// process is started at open and killed at close.
    /// </summary>
    public static SlcanFrameLink ForProcess(string fileName, string arguments, IClock? clock = null) =>
        new(() =>
        {
            var psi = new ProcessStartInfo(fileName, arguments)
            {
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = false,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            var p = Process.Start(psi) ?? throw new IOException($"cannot start {fileName}");
            return new ProcessStream(p);
        }, $"stdio:{fileName} {arguments}".TrimEnd(), clock);

    // ── Open / close ──────────────────────────────────────────────────────────
    public void Open()
    {
        Close();
        if (_streamFactory is not null)
        {
            _stream = _streamFactory();
        }
        else
        {
            string port = _portResolver?.Invoke() ?? throw new IOException("no CAN adapter port found");
            Description = port;
            // With USB CDC the baud rate means nothing; any valid value will do.
            _port = new SerialPort(port, 115200, Parity.None, 8, StopBits.One)
            {
                ReadTimeout = 200,
                WriteTimeout = 1000,
                DtrEnable = AssertDtrRts,
                RtsEnable = AssertDtrRts,
            };
            _port.Open();
            _stream = _port.BaseStream;
        }

        // Bring the adapter into a known state: close, 250 kbit/s, open.
        WriteRaw("C"); Thread.Sleep(30);
        WriteRaw("S5"); Thread.Sleep(30);
        WriteRaw("O"); Thread.Sleep(30);

        _rxRun = true;
        _open = true;
        _rxThread = new Thread(RxLoop) { IsBackground = true, Name = "slcan-rx" };
        _rxThread.Start();
        // Replies to the three commands above are not counted against any burst.
        Thread.Sleep(20);
        Interlocked.Exchange(ref _acks, 0);
        Interlocked.Exchange(ref _bells, 0);
    }

    public void Close()
    {
        bool wasOpen = _open;
        _open = false;
        _rxRun = false;
        try { if (_stream is not null) WriteRaw("C"); } catch { /* adapter already gone */ }
        try { _port?.Close(); } catch { }
        try { _stream?.Dispose(); } catch { }
        try { _port?.Dispose(); } catch { }
        try { _rxThread?.Join(500); } catch { }
        _rxThread = null;
        _port = null;
        _stream = null;
        if (wasOpen) LinkEvent?.Invoke(LinkEventKind.Closed, "closed", _clock.NowUs);
    }

    public void Dispose()
    {
        Close();
        _replySignal.Dispose();
    }

    // ── Send ──────────────────────────────────────────────────────────────────
    public void SendBurst(IReadOnlyList<CanFrame> frames)
    {
        if (!_open || _stream is null) throw new IOException("CAN link is not open");
        if (frames.Count == 0) return;
        try
        {
            lock (_txLock)
            {
                if (TxMode == LinkTxMode.Normal)
                {
                    // All frames of one message in ONE write (6.4): 32 × 27 = 864 bytes.
                    var buf = new byte[frames.Count * 27];
                    int n = 0;
                    foreach (var f in frames) n += Format(f, buf.AsSpan(n));
                    _stream.Write(buf, 0, n);
                    _stream.Flush();
                    FramesSent += frames.Count;
                }
                else
                {
                    foreach (var f in frames) SendStopAndWait(f);
                }
            }
        }
        catch (Exception ex) when (ex is IOException or TimeoutException or InvalidOperationException or UnauthorizedAccessException or ObjectDisposedException)
        {
            Fail(ex.Message);
            throw new IOException("CAN link write failed: " + ex.Message, ex);
        }
    }

    private void SendStopAndWait(CanFrame f)
    {
        var buf = new byte[27];
        int n = Format(f, buf);
        for (int attempt = 0; attempt < 50; attempt++)
        {
            long acks = Interlocked.Read(ref _acks), bells = Interlocked.Read(ref _bells);
            _replySignal.Reset();
            _stream!.Write(buf, 0, n);
            _stream.Flush();
            // Wait for the adapter's reply, at most 20 ms.
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < 20)
            {
                if (Interlocked.Read(ref _acks) > acks || Interlocked.Read(ref _bells) > bells) break;
                _replySignal.WaitOne(2);
            }
            if (Interlocked.Read(ref _bells) <= bells) { FramesSent++; return; }
            // BELL: the ring was full. Wait 2 ms and send the same frame again.
            SpinFor(2_000);
        }
        throw new IOException("adapter refused a frame 50 times (stop-and-wait)");
    }

    private static void SpinFor(long us)
    {
        long end = Stopwatch.GetTimestamp() + us * Stopwatch.Frequency / 1_000_000;
        while (Stopwatch.GetTimestamp() < end) Thread.SpinWait(20);
    }

    /// <summary>Formats one frame as <c>T&lt;id8&gt;&lt;dlc&gt;&lt;data&gt;\r</c>; returns the byte count.</summary>
    public static int Format(CanFrame f, Span<byte> dst)
    {
        int n = 0;
        dst[n++] = (byte)'T';
        uint id = f.Id & 0x1FFF_FFFF;
        for (int i = 7; i >= 0; i--) dst[n++] = Hex((int)(id >> (4 * i)) & 0xF);
        dst[n++] = (byte)('0' + f.Length);
        foreach (byte b in f.Data) { dst[n++] = Hex(b >> 4); dst[n++] = Hex(b & 0xF); }
        dst[n++] = (byte)'\r';
        return n;
    }

    /// <summary>Parses a received <c>T</c> line (without CR). Extra characters (timestamps) are ignored.</summary>
    public static bool TryParse(ReadOnlySpan<char> s, out CanFrame frame)
    {
        frame = default;
        if (s.Length < 10 || s[0] != 'T') return false;
        if (!uint.TryParse(s.Slice(1, 8), System.Globalization.NumberStyles.HexNumber, null, out uint id)) return false;
        int dlc = s[9] - '0';
        if (dlc < 0 || dlc > 8 || s.Length < 10 + 2 * dlc) return false;
        var data = new byte[dlc];
        for (int i = 0; i < dlc; i++)
        {
            int hi = FromHex(s[10 + 2 * i]), lo = FromHex(s[11 + 2 * i]);
            if (hi < 0 || lo < 0) return false;
            data[i] = (byte)((hi << 4) | lo);
        }
        frame = new CanFrame(id, data);
        return true;
    }

    private static byte Hex(int v) => (byte)(v < 10 ? '0' + v : 'A' + v - 10);

    private static int FromHex(char c) =>
        c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

    private void WriteRaw(string cmd)
    {
        byte[] b = Encoding.ASCII.GetBytes(cmd + "\r");
        lock (_txLock) { _stream!.Write(b, 0, b.Length); _stream.Flush(); }
    }

    private void Fail(string why)
    {
        if (!_open) return;
        LinkEvent?.Invoke(LinkEventKind.Error, why, _clock.NowUs);
    }

    // ── Receive ───────────────────────────────────────────────────────────────
    private void RxLoop()
    {
        var buf = new byte[4096];
        var line = new StringBuilder(40);
        var stream = _stream;
        string? why = null;
        while (_rxRun && stream is not null)
        {
            int n;
            try { n = stream.Read(buf, 0, buf.Length); }
            catch (TimeoutException) { continue; }
            catch (Exception ex) { why = ex.Message; break; }
            if (n <= 0) { why = "end of stream"; break; }
            for (int i = 0; i < n; i++)
            {
                byte b = buf[i];
                if (b == 0x07)
                {
                    Interlocked.Increment(ref _bells);
                    _replySignal.Set();
                    LinkEvent?.Invoke(LinkEventKind.Bell, null, _clock.NowUs);
                    line.Clear();
                    continue;
                }
                if (b == '\r' || b == '\n')
                {
                    if (line.Length == 0 || line[0] == 'z' || line[0] == 'Z')
                    {
                        // A bare CR (or Lawicel's z/Z) acknowledges one command.
                        if (b == '\r') { Interlocked.Increment(ref _acks); _replySignal.Set(); }
                    }
                    else if (TryParse(line.ToString().AsSpan(), out var f))
                    {
                        FramesReceived++;
                        FrameReceived?.Invoke(f, _clock.NowUs);
                    }
                    line.Clear();
                    continue;
                }
                if (line.Length < 64) line.Append((char)b);
            }
        }
        if (_rxRun && _open)
        {
            // The adapter went away under us (unplugged, bridge closed).
            _open = false;
            LinkEvent?.Invoke(LinkEventKind.Closed, why ?? "receive loop ended", _clock.NowUs);
        }
    }

    /// <summary>A network stream that also owns its TcpClient.</summary>
    private sealed class OwningStream(Stream inner, IDisposable owner) : Stream
    {
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override void Flush() => inner.Flush();
        public override int Read(byte[] buffer, int offset, int count) => inner.Read(buffer, offset, count);
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => inner.Write(buffer, offset, count);
        protected override void Dispose(bool disposing)
        {
            if (disposing) { inner.Dispose(); owner.Dispose(); }
            base.Dispose(disposing);
        }
    }

    /// <summary>Reads a child's stdout, writes its stdin, kills it on dispose.</summary>
    private sealed class ProcessStream(Process p) : Stream
    {
        private readonly Stream _out = p.StandardOutput.BaseStream;
        private readonly Stream _in = p.StandardInput.BaseStream;
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override void Flush() => _in.Flush();
        public override int Read(byte[] buffer, int offset, int count) => _out.Read(buffer, offset, count);
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => _in.Write(buffer, offset, count);
        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                try { _in.Dispose(); } catch { }
                try { if (!p.HasExited) p.Kill(entireProcessTree: true); } catch { }
                try { p.Dispose(); } catch { }
            }
            base.Dispose(disposing);
        }
    }
}
