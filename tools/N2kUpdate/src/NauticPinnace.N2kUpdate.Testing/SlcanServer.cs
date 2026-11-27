using System.Net;
using System.Net.Sockets;
using System.Text;
using NauticPinnace.N2kUpdate.Can;

namespace NauticPinnace.N2kUpdate.Testing;

/// <summary>
/// The adapter side of an slcan-over-TCP bridge: a TCP server that behaves like a
/// CANable towards its client. Every command line is answered with CR, received
/// <c>T</c> lines become frames of this link, frames sent through this link go to the
/// client as <c>T</c> lines. The CLI hosts the scripted display behind it
/// (<c>n2kupd sim-display</c>), so any slcan-over-TCP client - the C# sender, or a
/// C++ tool built for the cross test - can talk to a display without hardware.
/// One client at a time; a new client replaces the old one. With
/// <see cref="ForStdio"/> the same adapter speaks over stdin and stdout instead, for
/// the <c>stdio:</c> link of the sender.
/// </summary>
public sealed class SlcanServerLink : ICanFrameLink
{
    private readonly int _port;
    private readonly IClock _clock;
    private readonly object _tx = new();
    private readonly (Stream In, Stream Out)? _stdio;
    private TcpListener? _listener;
    private Thread? _accept;
    private volatile Stream? _client;
    private volatile bool _run;

    public SlcanServerLink(int port, IClock? clock = null)
    {
        _port = port;
        _clock = clock ?? SystemClock.Instance;
    }

    private SlcanServerLink(Stream input, Stream output, IClock? clock)
    {
        _stdio = (input, output);
        _clock = clock ?? SystemClock.Instance;
    }

    /// <summary>The adapter side over two streams, typically Console.OpenStandardInput/Output.</summary>
    public static SlcanServerLink ForStdio(Stream input, Stream output, IClock? clock = null) => new(input, output, clock);

    public bool IsOpen => _run;
    public LinkTxMode TxMode { get; set; }
    public int Port => (_listener?.LocalEndpoint as IPEndPoint)?.Port ?? _port;
    public bool HasClient => _client is not null;

    public event FrameReceivedHandler? FrameReceived;
    public event LinkEventHandler? LinkEvent;
    public event Action<string>? Log;

    public void Open()
    {
        if (_stdio is { } io)
        {
            _run = true;
            _client = io.Out;
            var t = new Thread(() => ReadLoop(io.In, io.Out, null)) { IsBackground = true, Name = "slcan-stdio-rx" };
            t.Start();
            return;
        }
        _listener = new TcpListener(IPAddress.Loopback, _port);
        _listener.Start();
        _run = true;
        _accept = new Thread(AcceptLoop) { IsBackground = true, Name = "slcan-server" };
        _accept.Start();
    }

    public void Close()
    {
        _run = false;
        try { _listener?.Stop(); } catch { }
        try { _client?.Dispose(); } catch { }
        _client = null;
    }

    public void Dispose() => Close();

    public void SendBurst(IReadOnlyList<CanFrame> frames)
    {
        var c = _client;
        if (c is null) return;   // nobody listening: the frames are lost, like on an empty bus
        var buf = new byte[frames.Count * 27];
        int n = 0;
        foreach (var f in frames) n += SlcanFrameLink.Format(f, buf.AsSpan(n));
        try { lock (_tx) { c.Write(buf, 0, n); c.Flush(); } }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
    }

    private void AcceptLoop()
    {
        while (_run)
        {
            TcpClient tcp;
            try { tcp = _listener!.AcceptTcpClient(); }
            catch (SocketException) { break; }
            catch (ObjectDisposedException) { break; }
            tcp.NoDelay = true;
            try { _client?.Dispose(); } catch { }
            var stream = tcp.GetStream();
            _client = stream;
            Log?.Invoke($"client connected from {tcp.Client.RemoteEndPoint}");
            var t = new Thread(() => ReadLoop(stream, stream, tcp)) { IsBackground = true, Name = "slcan-server-rx" };
            t.Start();
        }
    }

    private void ReadLoop(Stream s, Stream w, TcpClient? tcp)
    {
        var buf = new byte[4096];
        var line = new StringBuilder(40);
        try
        {
            while (_run)
            {
                int n = s.Read(buf, 0, buf.Length);
                if (n <= 0) break;
                for (int i = 0; i < n; i++)
                {
                    char c = (char)buf[i];
                    if (c != '\r' && c != '\n') { if (line.Length < 64) line.Append(c); continue; }
                    if (line.Length == 0) continue;
                    string l = line.ToString();
                    line.Clear();
                    // Like a CANable: CR means "queued" for a frame, "done" for a command.
                    lock (_tx) { w.Write([(byte)'\r'], 0, 1); w.Flush(); }
                    if (l[0] == 'T' && SlcanFrameLink.TryParse(l, out var f))
                        FrameReceived?.Invoke(f, _clock.NowUs);
                }
            }
        }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
        finally
        {
            if (ReferenceEquals(_client, w)) _client = null;
            try { tcp?.Dispose(); } catch { }
            Log?.Invoke("client disconnected");
            LinkEvent?.Invoke(LinkEventKind.Closed, "client disconnected", _clock.NowUs);
        }
    }
}

/// <summary>Runs a scripted display in wall-clock time on its own thread.</summary>
public sealed class RealTimeDisplayHost : IDisposable
{
    private readonly ScriptedDisplay _display;
    private readonly AutoResetEvent _wake = new(false);
    private readonly Thread _thread;
    private readonly object _lock = new();
    private volatile bool _run = true;

    public RealTimeDisplayHost(ScriptedDisplay display, IClock clock)
    {
        _display = display;
        display.Received += () => _wake.Set();
        _thread = new Thread(() =>
        {
            while (_run)
            {
                long next;
                lock (_lock) next = _display.Step(clock.NowUs);
                long ms = Math.Clamp((next - clock.NowUs) / 1000, 1, 200);
                _wake.WaitOne((int)ms);
            }
        }) { IsBackground = true, Name = "display-sim" };
        _thread.Start();
    }

    /// <summary>Runs an action on the display with the stepping thread held off.</summary>
    public void With(Action<ScriptedDisplay> a) { lock (_lock) a(_display); }

    public void Dispose()
    {
        _run = false;
        _wake.Set();
        _thread.Join(1000);
        _wake.Dispose();
    }
}
