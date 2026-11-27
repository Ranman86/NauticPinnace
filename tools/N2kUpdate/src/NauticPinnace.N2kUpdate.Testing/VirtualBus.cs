using NauticPinnace.N2kUpdate.Can;

namespace NauticPinnace.N2kUpdate.Testing;

/// <summary>A clock that only moves when the simulation moves it.</summary>
public sealed class VirtualClock : IClock
{
    public long NowUs { get; set; }
}

/// <summary>
/// A CAN bus at 250 kbit/s on a virtual clock. Frames take their exact on-wire time
/// (6.1); when several nodes have frames waiting, the lowest identifier wins
/// arbitration. Every completed frame is delivered to every other node and, if a
/// recorder is attached, written to the frame log at its completion time.
/// </summary>
public sealed class VirtualBus
{
    private readonly List<VirtualLink> _links = new();
    private VirtualLink? _curLink;
    private CanFrame _cur;
    private long _curEnd = long.MaxValue;

    public VirtualBus(VirtualClock clock) => Clock = clock;

    public VirtualClock Clock { get; }
    public double BitRate { get; set; } = 250_000;
    public CanLogWriter? Recorder { get; set; }

    /// <summary>Return true to lose a frame on the wire: nobody receives it, the log does not show it.</summary>
    public Func<CanFrame, VirtualLink, bool>? DropOnWire { get; set; }

    /// <summary>Return true to lose a frame at one receiver only (from, to).</summary>
    public Func<CanFrame, VirtualLink, VirtualLink, bool>? DropAtReceiver { get; set; }

    public long FramesOnWire { get; private set; }
    public long BitsOnWire { get; private set; }

    public VirtualLink Attach(string name, int adapterCapacity = int.MaxValue)
    {
        var l = new VirtualLink(this, name, adapterCapacity);
        _links.Add(l);
        return l;
    }

    /// <summary>Completion time of the frame on the wire, or long.MaxValue when idle.</summary>
    public long NextEventUs => _curEnd;

    internal void Kick()
    {
        if (_curLink is null) StartNext(Clock.NowUs);
    }

    /// <summary>Completes every frame that ends at or before <paramref name="nowUs"/>.</summary>
    public void Advance(long nowUs)
    {
        while (_curLink is not null && _curEnd <= nowUs)
        {
            var from = _curLink;
            var f = _cur;
            long end = _curEnd;
            // The adapter may have been closed while its frame was on the wire.
            if (from.Tx.Count > 0 && ReferenceEquals(from.Tx.Peek().Data, f.Data)) from.Tx.Dequeue();
            _curLink = null;
            _curEnd = long.MaxValue;
            FramesOnWire++;
            int bits = FrameBits.Exact(f);
            BitsOnWire += bits;
            from.BitsSent += bits;
            if (DropOnWire?.Invoke(f, from) != true)
            {
                Recorder?.Write(end, f);
                foreach (var to in _links)
                {
                    if (to == from || !to.IsOpen) continue;
                    if (DropAtReceiver?.Invoke(f, from, to) == true) continue;
                    to.Deliver(f, end);
                }
            }
            StartNext(end);
        }
    }

    private void StartNext(long at)
    {
        VirtualLink? best = null;
        foreach (var l in _links)
        {
            if (l.Tx.Count == 0) continue;
            if (best is null || l.Tx.Peek().Id < best.Tx.Peek().Id) best = l;
        }
        if (best is null) return;
        _curLink = best;
        _cur = best.Tx.Peek();
        long start = Math.Max(at, Clock.NowUs);
        _curEnd = start + (long)Math.Ceiling(FrameBits.Exact(_cur) * 1_000_000.0 / BitRate);
    }
}

/// <summary>
/// One node's adapter on the virtual bus. <paramref name="capacity"/> models the
/// adapter's transmit buffer: a CANable holds 63 + 3 frames (6.4); frames written
/// into a full buffer are dropped with a BELL, unless the link is in stop-and-wait
/// mode, where the writer waits for room (modelled as never dropping).
/// </summary>
public sealed class VirtualLink : ICanFrameLink
{
    private readonly VirtualBus _bus;
    private bool _open;

    internal VirtualLink(VirtualBus bus, string name, int capacity)
    {
        _bus = bus;
        Name = name;
        Capacity = capacity;
    }

    internal readonly Queue<CanFrame> Tx = new();

    public string Name { get; }
    public int Capacity { get; set; }
    public bool IsOpen => _open;
    public LinkTxMode TxMode { get; set; }

    /// <summary>While true, Open() fails (the adapter is unplugged).</summary>
    public bool Unplugged { get; set; }

    public long FramesWritten { get; private set; }
    public long BellCount { get; private set; }
    public long BitsSent { get; internal set; }
    public int MaxQueued { get; private set; }
    public int Queued => Tx.Count;

    public event FrameReceivedHandler? FrameReceived;
    public event LinkEventHandler? LinkEvent;

    public void Open()
    {
        if (Unplugged) throw new IOException($"{Name}: adapter unplugged");
        _open = true;
    }

    /// <summary>Like a real adapter, closing an open link reports Closed.</summary>
    public void Close()
    {
        if (!_open) return;
        _open = false;
        Tx.Clear();
        LinkEvent?.Invoke(LinkEventKind.Closed, "closed", _bus.Clock.NowUs);
    }

    /// <summary>The next write fails with an IOException (a USB error), the link stays open.</summary>
    public bool FailNextWrite { get; set; }

    /// <summary>Pulls the plug: the link closes and reports it.</summary>
    public void Unplug()
    {
        Unplugged = true;
        if (!_open) return;
        _open = false;
        Tx.Clear();
        LinkEvent?.Invoke(LinkEventKind.Closed, "unplugged", _bus.Clock.NowUs);
    }

    public void Dispose() => Close();

    public void SendBurst(IReadOnlyList<CanFrame> frames)
    {
        if (!_open) throw new IOException($"{Name}: link not open");
        if (FailNextWrite)
        {
            FailNextWrite = false;
            LinkEvent?.Invoke(LinkEventKind.Error, "write failed", _bus.Clock.NowUs);
            throw new IOException($"{Name}: write failed");
        }
        foreach (var f in frames)
        {
            if (TxMode == LinkTxMode.Normal && Tx.Count >= Capacity)
            {
                BellCount++;
                LinkEvent?.Invoke(LinkEventKind.Bell, null, _bus.Clock.NowUs);
                continue;
            }
            Tx.Enqueue(f);
            FramesWritten++;
        }
        MaxQueued = Math.Max(MaxQueued, Tx.Count);
        _bus.Kick();
    }

    internal void Deliver(CanFrame f, long ts) => FrameReceived?.Invoke(f, ts);
}

/// <summary>A participant of a simulation: runs whatever is due, returns its next wake-up.</summary>
public interface ISimNode
{
    long Step(long nowUs);
}

/// <summary>Wraps the sender engine as a simulation node.</summary>
public sealed class EngineNode(Engine.N2kEngine engine) : ISimNode
{
    public Engine.N2kEngine Engine { get; } = engine;
    public long Step(long nowUs) => Engine.Step();
}

/// <summary>Foreign traffic at a given bus load: single 8-byte frames of priority 3 (they win arbitration).</summary>
public sealed class TrafficSource : ISimNode
{
    private readonly VirtualLink _link;
    private readonly int _source;
    private long _next;
    private int _k;

    public TrafficSource(VirtualBus bus, int source, double load)
    {
        _link = bus.Attach($"traffic-{source}");
        _link.Open();
        _source = source;
        Load = load;
    }

    public double Load { get; set; }

    public long Step(long nowUs)
    {
        if (Load <= 0) return long.MaxValue;
        while (nowUs >= _next)
        {
            // PGN 130306 (wind), PDU2, priority 3.
            var data = new byte[8];
            data[0] = (byte)_k++;
            data[1] = (byte)(_k >> 3);
            for (int i = 2; i < 8; i++) data[i] = (byte)(0x55 ^ (_k * 31 + i));
            var f = new CanFrame(CanId.Encode(3, 130306, _source, 255), data);
            _link.SendBurst([f]);
            double us = FrameBits.Exact(f) * 1_000_000.0 / 250_000 / Load;
            _next = (_next == 0 ? nowUs : _next) + (long)us;
        }
        return _next;
    }
}

/// <summary>
/// Drives nodes and the bus in virtual time: run everything due at the current
/// instant, then jump to the earliest next event.
/// </summary>
public sealed class Simulation
{
    public Simulation()
    {
        Clock = new VirtualClock();
        Bus = new VirtualBus(Clock);
    }

    public VirtualClock Clock { get; }
    public VirtualBus Bus { get; }
    public List<ISimNode> Nodes { get; } = new();

    public long NowUs => Clock.NowUs;

    /// <summary>Runs until <paramref name="done"/> holds or the time limit is reached; returns whether done.</summary>
    public bool RunUntil(Func<bool> done, long limitUs)
    {
        int sameInstant = 0;
        long lastNow = -1;
        while (!done())
        {
            long now = Clock.NowUs;
            Bus.Advance(now);
            long next = long.MaxValue;
            ISimNode? eager = null;
            foreach (var n in Nodes)
            {
                long t = n.Step(now);
                if (t <= now) eager = n;
                next = Math.Min(next, t);
            }
            Bus.Advance(now);
            next = Math.Min(next, Bus.NextEventUs);
            if (done()) return true;
            if (next <= now)
            {
                // Something keeps asking to run at the same instant: a bug, not a slow test.
                if (now == lastNow && ++sameInstant > 10_000)
                    throw new InvalidOperationException($"livelock at {now} µs: {eager?.GetType().Name ?? "bus"} keeps returning a wake-up time in the past");
                lastNow = now;
                continue;
            }
            sameInstant = 0;
            lastNow = now;
            if (next > limitUs) { Clock.NowUs = limitUs; return done(); }
            Clock.NowUs = next;
        }
        return true;
    }

    /// <summary>Runs for a span of virtual time.</summary>
    public void RunFor(long us)
    {
        long end = Clock.NowUs + us;
        RunUntil(() => Clock.NowUs >= end, end);
    }
}
