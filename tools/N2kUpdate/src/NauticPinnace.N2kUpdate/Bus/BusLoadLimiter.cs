namespace NauticPinnace.N2kUpdate.Bus;

/// <summary>Bus-load settings (12.5).</summary>
public sealed record BusLoadOptions
{
    public const double MinShare = 0.10, MaxShare = 0.60, DefaultShare = 0.30;

    /// <summary>The allowed share s of 250 kbit/s, 0.10-0.60 (6.2).</summary>
    public double BusShare { get; init; } = DefaultShare;

    /// <summary>Halve the share under foreign load, recover slowly (6.3).</summary>
    public bool AdaptiveBackoff { get; init; } = true;

    /// <summary>Charge every frame 160 bits instead of its exact length (6.2).</summary>
    public bool ConservativeFrameBits { get; init; }

    public double ClampedShare => Math.Clamp(BusShare, MinShare, MaxShare);
}

/// <summary>One second of bus accounting, as reported to the UI (6.3).</summary>
public readonly record struct BusLoadSample(double OwnLoad, double ForeignLoad, double EffectiveShare, int Bells)
{
    public double TotalLoad => OwnLoad + ForeignLoad;
}

/// <summary>
/// The sender's bus-load limiter (section 6): the token bucket of 6.2, the adaptive
/// back-off of 6.3, the adapter drain estimate and the BELL rule of 6.4. Pure
/// arithmetic over microsecond timestamps, so tests run it on a virtual clock.
/// </summary>
public sealed class BusLoadLimiter
{
    public const double BitRate = 250_000;

    /// <summary>Bucket capacity: one 32-frame message at 160 bits (6.2).</summary>
    public const double BucketCapacity = 5_120;

    /// <summary>Frames that may be outstanding in the adapter after a DATA burst (6.4).</summary>
    public const int MaxBacklogFrames = 48;

    /// <summary>A control message is never delayed by more than this (6.2).</summary>
    public const long ControlMaxDelayUs = 50_000;

    private const long SecondUs = 1_000_000;

    private readonly BusLoadOptions _opt;
    private double _tokens = BucketCapacity;
    private long _tokensAt;
    private double _backlog;
    private long _backlogAt;
    private double _meanFrameBits = 138;

    // Current one-second bucket.
    private long _secStart;
    private long _secOwnBits, _secRxBits, _secOwnFrames;
    private int _secBells;

    // Adaptive back-off state.
    private int _highSeconds, _lowSeconds;
    private readonly Queue<(long Frames, int Bells)> _bellWindow = new();

    public BusLoadLimiter(BusLoadOptions options, long nowUs)
    {
        _opt = options;
        ConfiguredShare = options.ClampedShare;
        EffectiveShare = ConfiguredShare;
        _tokensAt = _backlogAt = _secStart = nowUs;
    }

    public double ConfiguredShare { get; }

    /// <summary>s_eff: the share currently in force after back-off (6.3).</summary>
    public double EffectiveShare { get; private set; }

    public bool Conservative => _opt.ConservativeFrameBits;

    /// <summary>The last complete second.</summary>
    public BusLoadSample Last { get; private set; }

    /// <summary>Foreign load of the last complete second, capped at 0.9 for the drain estimate.</summary>
    public double ForeignLoadForDrain => Math.Min(0.9, Last.ForeignLoad);

    /// <summary>Raised once per second with the sample just completed.</summary>
    public event Action<BusLoadSample>? Sampled;

    public long TotalOwnBits { get; private set; }
    public long TotalOwnFrames { get; private set; }

    private double Rate => EffectiveShare * BitRate;

    // ── Token bucket ──────────────────────────────────────────────────────────
    private void Refill(long nowUs)
    {
        if (nowUs <= _tokensAt) return;
        _tokens = Math.Min(BucketCapacity, _tokens + (nowUs - _tokensAt) * Rate / SecondUs);
        _tokensAt = nowUs;
    }

    /// <summary>Frames still in the adapter, by the drain estimate.</summary>
    public double Backlog(long nowUs)
    {
        DrainTo(nowUs);
        return _backlog;
    }

    private double DrainRatePerUs => BitRate * (1 - ForeignLoadForDrain) / _meanFrameBits / SecondUs;

    private void DrainTo(long nowUs)
    {
        if (nowUs <= _backlogAt) return;
        _backlog = Math.Max(0, _backlog - (nowUs - _backlogAt) * DrainRatePerUs);
        _backlogAt = nowUs;
    }

    /// <summary>
    /// The earliest time a DATA burst of <paramref name="frames"/> frames costing
    /// <paramref name="costBits"/> may be written: when the bucket holds the cost, and
    /// when backlog + n ≤ 48 (6.2, 6.4).
    /// </summary>
    public long DataReadyAt(int costBits, int frames, long nowUs)
    {
        Refill(nowUs);
        DrainTo(nowUs);
        long t = nowUs;
        if (_tokens < costBits)
            t = nowUs + (long)Math.Ceiling((costBits - _tokens) * SecondUs / Rate);
        double over = _backlog + frames - MaxBacklogFrames;
        if (over > 0)
            t = Math.Max(t, nowUs + (long)Math.Ceiling(over / DrainRatePerUs));
        return t;
    }

    /// <summary>The time a control message may go: at once if the bucket allows, else at most 50 ms later.</summary>
    public long ControlDueAt(int costBits, long nowUs)
    {
        Refill(nowUs);
        if (_tokens >= costBits) return nowUs;
        long wait = (long)Math.Ceiling((costBits - _tokens) * SecondUs / Rate);
        return nowUs + Math.Min(ControlMaxDelayUs, wait);
    }

    /// <summary>Charges a burst that was written: tokens (may go negative for control), backlog, meter.</summary>
    public void Charge(int costBits, int frames, int exactBits, long nowUs)
    {
        Refill(nowUs);
        DrainTo(nowUs);
        _tokens -= costBits;
        _backlog += frames;
        if (frames > 0) _meanFrameBits = 0.95 * _meanFrameBits + 0.05 * ((double)exactBits / frames);
        _secOwnBits += exactBits;
        _secOwnFrames += frames;
        TotalOwnBits += exactBits;
        TotalOwnFrames += frames;
    }

    /// <summary>The adapter is known to be empty (just opened, or stop-and-wait confirmed every frame).</summary>
    public void ResetBacklog(long nowUs) { _backlog = 0; _backlogAt = nowUs; }

    // ── Meter (6.3) ───────────────────────────────────────────────────────────
    /// <summary>A frame received from the bus (the CANable sees every frame).</summary>
    public void OnReceived(int exactBits) => _secRxBits += exactBits;

    public void OnBell() => _secBells++;

    /// <summary>Closes elapsed one-second buckets; returns the next bucket boundary.</summary>
    public long Tick(long nowUs)
    {
        while (nowUs >= _secStart + SecondUs)
        {
            CloseSecond();
            _secStart += SecondUs;
        }
        return _secStart + SecondUs;
    }

    private void CloseSecond()
    {
        var s = new BusLoadSample(_secOwnBits / BitRate, _secRxBits / BitRate, EffectiveShare, _secBells);
        _bellWindow.Enqueue((_secOwnFrames, _secBells));
        while (_bellWindow.Count > 10) _bellWindow.Dequeue();
        _secOwnBits = _secRxBits = _secOwnFrames = 0;
        _secBells = 0;
        Last = s;

        if (_opt.AdaptiveBackoff)
        {
            double total = s.TotalLoad;
            _highSeconds = total > 0.70 ? _highSeconds + 1 : 0;
            _lowSeconds = total < 0.60 ? _lowSeconds + 1 : 0;
            if (_highSeconds >= 2)
            {
                SetShare(Math.Max(BusLoadOptions.MinShare, EffectiveShare / 2));
                _highSeconds = 0;
            }
            else if (_lowSeconds >= 5)
            {
                SetShare(Math.Min(ConfiguredShare, EffectiveShare + 0.05));
                _lowSeconds = 0;
            }
        }

        // BELLs above 1 % of our frames over 10 s halve the share (6.4).
        long frames = 0; int bells = 0;
        foreach (var (f, b) in _bellWindow) { frames += f; bells += b; }
        if (bells > 0 && frames > 0 && bells > 0.01 * frames)
        {
            SetShare(Math.Max(BusLoadOptions.MinShare, EffectiveShare / 2));
            _bellWindow.Clear();
        }
        Sampled?.Invoke(Last);
    }

    private void SetShare(double s)
    {
        // Refill at the old rate up to now first, so the change is not retroactive.
        Refill(_secStart + SecondUs);
        EffectiveShare = Math.Round(s, 4);
    }
}
