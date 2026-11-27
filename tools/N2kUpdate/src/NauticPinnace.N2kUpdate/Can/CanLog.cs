using System.Globalization;
using System.Text;

namespace NauticPinnace.N2kUpdate.Can;

/// <summary>
/// The frame log of the cross test (14.3.2): one frame per line,
/// <c>t_ms id29 dlc hex</c>, for example <c>1234 1DEF2AE6 8 00DFFE9FA45A0000</c>.
///   t_ms  integer milliseconds since the start of the log (frame order = line order)
///   id29  the 29-bit CAN identifier, 8 upper-case hex digits
///   dlc   decimal 0..8
///   hex   dlc data bytes, upper-case hex, no separators
/// Both directions go into one log; the source address in the identifier tells them
/// apart. There are no comment lines.
/// </summary>
public sealed class CanLogWriter : IDisposable
{
    private readonly TextWriter _w;
    private readonly object _lock = new();
    private readonly long _t0Us;
    private readonly StringBuilder _sb = new(40);

    public long Lines { get; private set; }

    public CanLogWriter(string path, long t0Us = 0)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        _w = new StreamWriter(path, false, new UTF8Encoding(false), 1 << 16) { NewLine = "\n" };
        _t0Us = t0Us;
    }

    public CanLogWriter(TextWriter writer, long t0Us = 0)
    {
        _w = writer;
        _t0Us = t0Us;
    }

    public void Write(long timestampUs, CanFrame f)
    {
        lock (_lock)
        {
            _sb.Clear();
            long ms = Math.Max(0, (timestampUs - _t0Us) / 1000);
            _sb.Append(ms.ToString(CultureInfo.InvariantCulture)).Append(' ')
               .Append(f.Id.ToString("X8")).Append(' ')
               .Append(f.Length.ToString(CultureInfo.InvariantCulture)).Append(' ')
               .Append(Convert.ToHexString(f.Data));
            _w.WriteLine(_sb);
            Lines++;
        }
    }

    public void Flush() { lock (_lock) _w.Flush(); }

    public void Dispose() { lock (_lock) _w.Dispose(); }
}

/// <summary>Reads a frame log written by <see cref="CanLogWriter"/>.</summary>
public static class CanLogReader
{
    public static IEnumerable<(long TimeMs, CanFrame Frame)> Read(string path)
    {
        foreach (string raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.Length == 0) continue;
            var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 3) continue;
            long t = long.Parse(parts[0], CultureInfo.InvariantCulture);
            uint id = uint.Parse(parts[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            int dlc = int.Parse(parts[2], CultureInfo.InvariantCulture);
            byte[] data = dlc == 0 ? [] : Convert.FromHexString(parts[3]);
            if (data.Length != dlc) throw new FormatException($"dlc {dlc} does not match the data in: {line}");
            yield return (t, new CanFrame(id, data));
        }
    }
}

/// <summary>
/// Wraps any link and writes every frame it sends or receives to a frame log, so a
/// live run (CANable, TCP bridge) can be replayed later.
/// </summary>
public sealed class RecordingFrameLink : ICanFrameLink
{
    private readonly ICanFrameLink _inner;
    private readonly CanLogWriter _log;
    private readonly IClock _clock;

    public RecordingFrameLink(ICanFrameLink inner, CanLogWriter log, IClock? clock = null)
    {
        _inner = inner;
        _log = log;
        _clock = clock ?? SystemClock.Instance;
        _inner.FrameReceived += (f, ts) => { _log.Write(ts, f); FrameReceived?.Invoke(f, ts); };
        _inner.LinkEvent += (k, d, ts) => LinkEvent?.Invoke(k, d, ts);
    }

    public bool IsOpen => _inner.IsOpen;
    public LinkTxMode TxMode { get => _inner.TxMode; set => _inner.TxMode = value; }
    public event FrameReceivedHandler? FrameReceived;
    public event LinkEventHandler? LinkEvent;

    public void Open() => _inner.Open();
    public void Close() { _inner.Close(); _log.Flush(); }

    public void SendBurst(IReadOnlyList<CanFrame> frames)
    {
        _inner.SendBurst(frames);
        long t = _clock.NowUs;
        foreach (var f in frames) _log.Write(t, f);
    }

    public void Dispose()
    {
        _inner.Dispose();
        _log.Dispose();
    }
}

/// <summary>
/// The DRY transport: no adapter. Every frame that is sent, and every frame that is
/// injected as received, goes into a frame log with its timestamp. The
/// <see cref="BurstSent"/> hook lets an in-process responder (a scripted display, a
/// bridge to an external receiver) see each burst and answer through
/// <see cref="Inject"/>; that is the loopback and replay hook.
/// </summary>
public sealed class DryFrameLink : ICanFrameLink
{
    private readonly IClock _clock;
    private readonly CanLogWriter? _log;
    private volatile bool _open;

    public DryFrameLink(IClock? clock = null, CanLogWriter? log = null)
    {
        _clock = clock ?? SystemClock.Instance;
        _log = log;
    }

    public bool IsOpen => _open;
    public LinkTxMode TxMode { get; set; }
    public long FramesSent { get; private set; }

    public event FrameReceivedHandler? FrameReceived;
    public event LinkEventHandler? LinkEvent;

    /// <summary>Called after each burst with the frames and the time they were "written".</summary>
    public event Action<IReadOnlyList<CanFrame>, long>? BurstSent;

    public void Open() => _open = true;

    public void Close()
    {
        if (!_open) return;
        _open = false;
        LinkEvent?.Invoke(LinkEventKind.Closed, "closed", _clock.NowUs);
    }

    public void Dispose() => Close();

    public void SendBurst(IReadOnlyList<CanFrame> frames)
    {
        if (!_open) throw new IOException("CAN link is not open");
        long t = _clock.NowUs;
        foreach (var f in frames) _log?.Write(t, f);
        FramesSent += frames.Count;
        BurstSent?.Invoke(frames, t);
    }

    /// <summary>Feeds a frame as if the adapter had received it.</summary>
    public void Inject(CanFrame f, long? timestampUs = null)
    {
        long t = timestampUs ?? _clock.NowUs;
        _log?.Write(t, f);
        FrameReceived?.Invoke(f, t);
    }

    /// <summary>Simulates an adapter BELL.</summary>
    public void InjectBell() => LinkEvent?.Invoke(LinkEventKind.Bell, null, _clock.NowUs);
}
