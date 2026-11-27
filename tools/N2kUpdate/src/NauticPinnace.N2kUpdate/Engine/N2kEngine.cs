using System.Collections.Concurrent;
using NauticPinnace.N2kUpdate.Bus;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;

namespace NauticPinnace.N2kUpdate.Engine;

/// <summary>Identity and bus settings of one sender node.</summary>
public sealed record EngineOptions
{
    /// <summary>The 21-bit unique number of the NAME, stored in the sender's settings (12.3).</summary>
    public int UniqueNumber { get; init; }

    /// <summary>230 (0xE6) for the standalone updater (12.3).</summary>
    public int PreferredAddress { get; init; } = 230;

    /// <summary>Answer for ISO requests of PGN 126996 (12.3: model "NauticPinnace Updater").</summary>
    public ProductInfo ProductInfo { get; init; } = new()
    {
        ProductCode = 2046,
        ModelId = "NauticPinnace Updater",
        SoftwareVersion = "1.0.0",
        ModelVersion = "",
        SerialCode = "",
    };

    public BusLoadOptions BusLoad { get; init; } = new();

    public N2kName Name => N2kName.ForUpdater(UniqueNumber);
}

/// <summary>
/// Something the engine runs: discovery, identify, one update job. All callbacks run
/// on the engine thread; <see cref="Tick"/> returns the next time it wants to run.
/// </summary>
public abstract class EngineOperation
{
    public bool IsFinished { get; protected set; }

    public virtual void OnMessage(UpdateMessage msg, int source, int destination, long nowUs) { }
    public virtual void OnProductInfo(int source, ProductInfo info, long nowUs) { }
    public virtual void OnLinkEvent(LinkEventKind kind, long nowUs) { }
    public virtual void OnAddressChanged(int oldAddress, int newAddress, long nowUs) { }
    public abstract long Tick(long nowUs);

    /// <summary>Called once when the caller cancels.</summary>
    public virtual void Cancel(long nowUs) { }

    /// <summary>Called once after the operation finished and was removed from the engine.</summary>
    public virtual void OnRemoved() { }

    /// <summary>
    /// The operation threw: it ends here. Overrides complete their caller with a
    /// failure; the engine itself keeps running.
    /// </summary>
    public virtual void OnCrash(Exception ex, long nowUs) => IsFinished = true;
}

/// <summary>
/// The sender's N2K node: one link, one claimed address, fast-packet framing in and
/// out, the bus-load limiter, and the operations that use them. Single-threaded by
/// design: <see cref="Step"/> runs everything and returns the next wake-up time,
/// so a thread (<see cref="EngineHost"/>) or a virtual-time test can drive it.
/// Only <see cref="Post"/> and the link callbacks may be called from other threads.
/// </summary>
public sealed class N2kEngine
{
    private readonly ConcurrentQueue<(CanFrame Frame, long Ts)> _rx = new();
    private readonly ConcurrentQueue<CanFrame> _externalTx = new();
    private readonly ConcurrentQueue<(LinkEventKind Kind, string? Detail, long Ts)> _events = new();
    private readonly ConcurrentQueue<Action> _commands = new();
    private readonly AutoResetEvent _wake = new(false);
    private readonly FastPacketAssembler _assembler = new();
    private readonly FastPacketSequencer _seq = new();
    private readonly List<EngineOperation> _ops = new();
    private readonly Queue<PendingControl> _control = new();
    private readonly Random _rng;
    private BusLoadLimiter _limiter;
    private int _lastSessionId;
    private long _bells;
    private long _offerProbeUs = -1;
    private long _offerProbeBells;
    private long _lastExpireUs;
    private long _reopenedAtUs = long.MinValue;
    private bool _stopAndWaitByProbe;
    private bool _linkLost;

    private sealed record PendingControl(int Destination, int Priority, int Pgn, byte[] Payload, long DueUs, bool IsOffer);

    /// <param name="link">The CAN adapter.</param>
    /// <param name="clock">Monotonic time (virtual in tests).</param>
    /// <param name="options">NAME, preferred address, product information, bus load.</param>
    /// <param name="rng">Session ids (3.3); tests pass a seeded one.</param>
    /// <param name="sharedSource">
    /// An address claimed by someone else on the same adapter (12.3: NauticPi's identity
    /// in slcan mode). The engine then claims nothing itself, answers no claim requests and
    /// sends with that address. Null: the engine claims its own address with the NAME of
    /// <paramref name="options"/>.
    /// </param>
    public N2kEngine(ICanFrameLink link, IClock clock, EngineOptions options, Random? rng = null,
                     ISourceAddress? sharedSource = null)
    {
        Link = link;
        Clock = clock;
        Options = options;
        _rng = rng ?? new Random();
        _limiter = new BusLoadLimiter(options.BusLoad, clock.NowUs);
        _limiter.Sampled += s => BusSampled?.Invoke(s);
        if (sharedSource is null)
        {
            Claimer = new AddressClaimer(options.Name, options.PreferredAddress);
            Source = Claimer;
            Claimer.Changed += AddressChanged;          // raised inside Step(), engine thread
        }
        else
        {
            Source = sharedSource;
            // Raised on its owner's thread: handed over to the engine thread.
            sharedSource.Changed += (o, n) => Post(() => AddressChanged(o, n));
        }
        link.FrameReceived += (f, ts) => { _rx.Enqueue((f, ts)); _wake.Set(); };
        link.LinkEvent += (k, d, ts) => { _events.Enqueue((k, d, ts)); _wake.Set(); };
    }

    public ICanFrameLink Link { get; }
    public IClock Clock { get; }
    public EngineOptions Options { get; }
    /// <summary>The engine's own address claim; null when it sends with a shared source.</summary>
    public AddressClaimer? Claimer { get; }
    public ISourceAddress Source { get; }
    public BusLoadLimiter Limiter => _limiter;
    public WaitHandle WakeHandle => _wake;

    /// <summary>The sessions this sender opened (RESUME only for its own, 5.3.4).</summary>
    public SessionMemory Sessions { get; } = new();

    /// <summary>The time the last DATA of another source was seen (6.5); long.MinValue if never.</summary>
    public long LastForeignDataUs { get; private set; } = long.MinValue;

    /// <summary>Total adapter BELLs since start.</summary>
    public long Bells => _bells;

    public bool StopAndWait => Link.TxMode == LinkTxMode.StopAndWait;

    public bool LinkLost
    {
        get => _linkLost;
        private set
        {
            if (_linkLost == value) return;
            _linkLost = value;
            LinkStateChanged?.Invoke(!value);
        }
    }

    /// <summary>Diagnostic messages (engine thread).</summary>
    public event Action<string>? Log;

    /// <summary>Raised once per second with the bus-load sample (engine thread).</summary>
    public event Action<BusLoadSample>? BusSampled;

    /// <summary>The adapter was lost (false) or is usable again (true) (engine thread).</summary>
    public event Action<bool>? LinkStateChanged;

    /// <summary>
    /// Frames this node's owner sent through the same adapter outside the engine (12.6:
    /// NauticPi's own traffic on a shared CANable). The adapter does not echo them, so they
    /// are counted here towards the total bus load of 6.3. Thread-safe.
    /// </summary>
    public void NoteExternalTransmit(CanFrame frame)
    {
        _externalTx.Enqueue(frame);
    }

    // The address this node's frames carry, also while a claim is still running.
    private int Me => Claimer?.Candidate ?? Source.Address;

    private void AddressChanged(int oldAddress, int newAddress)
    {
        Info($"source address {newAddress} claimed" + (oldAddress != N2kPgn.NullAddress ? $" (was {oldAddress})" : ""));
        foreach (var op in _ops.ToArray()) op.OnAddressChanged(oldAddress, newAddress, Clock.NowUs);
    }

    internal void Info(string msg) => Log?.Invoke(msg);

    /// <summary>Runs <paramref name="action"/> on the engine thread at the next step. Thread-safe.</summary>
    public void Post(Action action)
    {
        _commands.Enqueue(action);
        _wake.Set();
    }

    /// <summary>Replaces the bus-load settings (each update may bring its own, 12.5).</summary>
    public void SetBusLoad(BusLoadOptions options)
    {
        _limiter = new BusLoadLimiter(options, Clock.NowUs);
        _limiter.Sampled += s => BusSampled?.Invoke(s);
    }

    /// <summary>Opens the link if needed and starts the address claim.</summary>
    public void Start()
    {
        if (!Link.IsOpen) Link.Open();
        LinkLost = false;
        Claimer?.Start(Clock.NowUs);
        FlushClaims();
    }

    /// <summary>
    /// A session ended. Stop-and-wait, chosen because the adapter dropped frames of an
    /// OFFER, holds "for the rest of the session" (6.4): the next session probes again.
    /// </summary>
    public void SessionEnded()
    {
        if (!_stopAndWaitByProbe) return;
        _stopAndWaitByProbe = false;
        Link.TxMode = LinkTxMode.Normal;
    }

    /// <summary>A fresh session id: random 1..254, never the previous one (3.3).</summary>
    public byte NextSessionId()
    {
        int id;
        do id = _rng.Next(1, 255); while (id == _lastSessionId);
        _lastSessionId = id;
        return (byte)id;
    }

    public void Add(EngineOperation op)
    {
        _ops.Add(op);
        _wake.Set();
    }

    public IReadOnlyList<EngineOperation> Operations => _ops;

    // ── Step ──────────────────────────────────────────────────────────────────
    /// <summary>Processes everything that is due and returns the next wake-up time (µs).</summary>
    public long Step()
    {
        long now = Clock.NowUs;
        while (_commands.TryDequeue(out var cmd))
        {
            try { cmd(); }
            catch (Exception ex) { Info("command failed: " + ex.Message); }
        }

        while (_events.TryDequeue(out var ev))
        {
            if (ev.Kind == LinkEventKind.Bell) { _bells++; _limiter.OnBell(); }
            else if (ev.Ts <= _reopenedAtUs)
            {
                continue;   // raised by our own Close() inside TryReopenLink
            }
            else
            {
                LinkLost = true;
                Info($"CAN link {ev.Kind.ToString().ToLowerInvariant()}: {ev.Detail}");
            }
            foreach (var op in _ops.ToArray()) op.OnLinkEvent(ev.Kind, now);
        }

        while (_rx.TryDequeue(out var rx)) HandleFrame(rx.Frame, rx.Ts);
        while (_externalTx.TryDequeue(out var ext)) _limiter.OnReceived(FrameBits.Exact(ext));
        if (now - _lastExpireUs > 500_000) { _assembler.Expire(now); _lastExpireUs = now; }

        long next = long.MaxValue;
        if (Claimer is not null) next = Math.Min(next, Claimer.Tick(now));
        FlushClaims();
        next = Math.Min(next, _limiter.Tick(now));
        ProbeOffer(now);

        next = Math.Min(next, FlushControl(now));
        foreach (var op in _ops.ToArray())
        {
            if (op.IsFinished) continue;
            long t;
            try { t = op.Tick(now); }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                Info("operation failed: " + ex);
                op.OnCrash(ex, now);
                continue;
            }
            next = Math.Min(next, t);
        }
        next = Math.Min(next, FlushControl(now));
        for (int i = _ops.Count - 1; i >= 0; i--)
        {
            if (!_ops[i].IsFinished) continue;
            var done = _ops[i];
            _ops.RemoveAt(i);
            done.OnRemoved();
        }
        if (_offerProbeUs >= 0) next = Math.Min(next, _offerProbeUs + OfferProbeUs);
        return next;
    }

    // ── Receive ───────────────────────────────────────────────────────────────
    private void HandleFrame(CanFrame f, long ts)
    {
        _limiter.OnReceived(FrameBits.Exact(f));
        var (pgn, _, src, dst) = CanId.Decode(f.Id);
        int me = Me;
        if (src != me) Claimer?.NoteSource(src);

        // Another source's DATA, seen on frame 0 of 126720: FE 9F A4 at bytes 2-4 (6.5).
        // Checked before the destination filter: the transfer is to someone else.
        if (pgn == UpdProto.Pgn && src != me && f.Length >= 5 && (f.Data[0] & 0x1F) == 0 &&
            f.Data[2] == UpdProto.Mfg0 && f.Data[3] == UpdProto.Mfg1 && f.Data[4] == (byte)MsgType.Data)
            LastForeignDataUs = ts;

        if (pgn == N2kPgn.IsoAddressClaim)
        {
            if (f.Length >= 8) Claimer?.OnClaim(src, N2kName.Read(f.Data), ts);
            return;
        }

        // Drop PDU1 frames for other destinations before reassembly (12.2).
        if (CanId.IsPdu1(f.Id) && dst != me && dst != UpdProto.Broadcast) return;

        if (pgn == N2kPgn.IsoRequest)
        {
            if (f.Length >= 3) HandleRequest(f.Data[0] | (f.Data[1] << 8) | (f.Data[2] << 16), src);
            return;
        }

        if (!N2kPgn.IsFastPacket(pgn)) return;
        byte[]? payload = _assembler.Add(src, dst, pgn, f.Data, ts);
        if (payload is null) return;

        if (pgn == UpdProto.Pgn)
        {
            var msg = UpdateMessage.Decode(payload);
            if (msg is null) return;
            foreach (var op in _ops.ToArray())
            {
                if (op.IsFinished) continue;
                try { op.OnMessage(msg, src, dst, ts); }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    Info("operation failed: " + ex);
                    op.OnCrash(ex, ts);
                }
            }
        }
        else if (pgn == N2kPgn.ProductInfo)
        {
            var pi = ProductInfo.Decode(payload);
            if (pi is null) return;
            foreach (var op in _ops.ToArray()) if (!op.IsFinished) op.OnProductInfo(src, pi, ts);
        }
    }

    private void HandleRequest(int requested, int requester)
    {
        // With a shared source its owner answers for the node (claim and product info).
        if (Claimer is null) return;
        if (requested == N2kPgn.IsoAddressClaim) { Claimer.OnClaimRequest(); FlushClaims(); }
        else if (requested == N2kPgn.ProductInfo && Claimer.IsUsable)
            SendPgn(6, N2kPgn.ProductInfo, requester, Options.ProductInfo.Encode());
    }

    // ── Send ──────────────────────────────────────────────────────────────────
    private void FlushClaims()
    {
        if (Claimer is null) return;
        while (Claimer.Outbox.TryDequeue(out var c))
        {
            uint id = CanId.Encode(6, N2kPgn.IsoAddressClaim, c.Source, UpdProto.Broadcast);
            WriteFrames([new CanFrame(id, c.Name.ToBytes())]);
        }
    }

    /// <summary>Sends an ISO request (PGN 59904) for <paramref name="pgn"/>.</summary>
    public void SendIsoRequest(int pgn, int destination)
    {
        if (!Source.IsUsable) return;
        uint id = CanId.Encode(6, N2kPgn.IsoRequest, Source.Address, destination);
        WriteFrames([new CanFrame(id, [(byte)pgn, (byte)(pgn >> 8), (byte)(pgn >> 16)])]);
    }

    /// <summary>Sends any other PGN at once (fast packet for the fast-packet PGNs).</summary>
    public void SendPgn(int priority, int pgn, int destination, byte[] payload)
    {
        if (!Source.IsUsable) return;
        uint id = CanId.Encode(priority, pgn, Source.Address, destination);
        if (N2kPgn.IsFastPacket(pgn))
            WriteFrames(FastPacket.Encode(id, payload, _seq.Take(Source.Address, pgn)));
        else
            WriteFrames([new CanFrame(id, payload)]);
    }

    /// <summary>
    /// Queues a control message of this protocol (priority 6). It goes at once if the
    /// token bucket allows, otherwise at most 50 ms later (6.2). Order is kept.
    /// </summary>
    public void SendControl(int destination, UpdateMessage msg)
    {
        byte[] payload = msg.Encode();
        int frames = FastPacket.FrameCount(payload.Length);
        int cost = frames * (Limiter.Conservative ? FrameBits.Conservative : 140);
        long due = _control.Count == 0 ? Limiter.ControlDueAt(cost, Clock.NowUs) : Clock.NowUs;
        _control.Enqueue(new PendingControl(destination, UpdProto.PrioControl, UpdProto.Pgn, payload, due, msg is OfferMsg));
        FlushControl(Clock.NowUs);
    }

    private long FlushControl(long now)
    {
        while (_control.Count > 0)
        {
            if (!Source.IsUsable || !Link.IsOpen) return now + 50_000;
            var c = _control.Peek();
            if (c.DueUs > now) return c.DueUs;
            _control.Dequeue();
            uint id = CanId.Encode(c.Priority, c.Pgn, Source.Address, c.Destination);
            var frames = FastPacket.Encode(id, c.Payload, _seq.Take(Source.Address, c.Pgn));
            if (c.IsOffer)
            {
                // A BELL to any frame of the OFFER, written while the adapter is idle,
                // means the adapter cannot hold one message: stop-and-wait (6.4).
                _offerProbeUs = Clock.NowUs;
                _offerProbeBells = _bells + PendingBells();
            }
            WriteFrames(frames);
        }
        return long.MaxValue;
    }

    private const long OfferProbeUs = 100_000;

    private long PendingBells() => _events.Count(e => e.Kind == LinkEventKind.Bell);

    private void ProbeOffer(long now)
    {
        if (_offerProbeUs < 0 || now < _offerProbeUs + OfferProbeUs) return;
        _offerProbeUs = -1;
        if (_bells > _offerProbeBells && Link.TxMode == LinkTxMode.Normal)
        {
            Link.TxMode = LinkTxMode.StopAndWait;
            _stopAndWaitByProbe = true;
            Info("the adapter dropped frames of the OFFER: its buffer is smaller than one message, switching to stop-and-wait");
        }
    }

    /// <summary>True while no control message waits; DATA never overtakes control.</summary>
    public bool ControlIdle => _control.Count == 0;

    /// <summary>
    /// Writes one DATA message as one burst if the limiter allows it now (6.2, 6.4).
    /// Otherwise returns false with the time to try again.
    /// </summary>
    public bool TrySendData(int destination, byte[] payload, out long retryAtUs)
    {
        long now = Clock.NowUs;
        if (!Source.IsUsable || !Link.IsOpen) { retryAtUs = now + 50_000; return false; }
        if (_control.Count > 0) { retryAtUs = Math.Max(now, _control.Peek().DueUs); return false; }
        uint id = CanId.Encode(UpdProto.PrioData, UpdProto.Pgn, Source.Address, destination);
        var frames = FastPacket.Encode(id, payload, _seq.Peek(Source.Address, UpdProto.Pgn));
        int cost = 0;
        foreach (var f in frames) cost += FrameBits.Cost(f, Limiter.Conservative);
        long ready = Limiter.DataReadyAt(cost, frames.Count, now);
        if (ready > now) { retryAtUs = ready; return false; }
        _seq.Take(Source.Address, UpdProto.Pgn);
        retryAtUs = now;
        return WriteFrames(frames, cost);
    }

    private bool WriteFrames(List<CanFrame> frames, int cost = -1)
    {
        if (!Link.IsOpen) return false;
        int exact = 0;
        foreach (var f in frames) exact += FrameBits.Exact(f);
        if (cost < 0) cost = Limiter.Conservative ? frames.Count * FrameBits.Conservative : exact;
        try
        {
            Link.SendBurst(frames);
        }
        catch (IOException ex)
        {
            LinkLost = true;
            Info("CAN write failed: " + ex.Message);
            return false;
        }
        Limiter.Charge(cost, frames.Count, exact, Clock.NowUs);
        if (Link.TxMode == LinkTxMode.StopAndWait) Limiter.ResetBacklog(Clock.NowUs);
        return true;
    }

    /// <summary>Closes and reopens the adapter (5.8). Returns true when it is open again.</summary>
    public bool TryReopenLink()
    {
        try
        {
            try { Link.Close(); } catch { /* already gone */ }
            Link.Open();
            _reopenedAtUs = Clock.NowUs;
            LinkLost = false;
            Limiter.ResetBacklog(Clock.NowUs);
            _assembler.Clear();
            // The bus may have changed while we were away: claim our address again.
            Claimer?.OnClaimRequest();
            FlushClaims();
            Info("CAN link reopened");
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or ArgumentException or TimeoutException)
        {
            return false;
        }
    }
}
