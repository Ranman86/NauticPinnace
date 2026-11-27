using NauticPinnace.N2kUpdate.Bus;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Session;

namespace NauticPinnace.N2kUpdate;

/// <summary>
/// Runs an <see cref="N2kEngine"/> on its own thread: step, then wait until the next
/// wake-up time or until a frame or command arrives. The wait goes through the
/// injected <see cref="IHostWaiter"/> (6.6: high-resolution timer on Windows).
/// </summary>
public sealed class EngineHost : IDisposable
{
    private readonly N2kEngine _engine;
    private readonly IHostWaiter _waiter;
    private readonly Thread _thread;
    private volatile bool _run = true;

    public EngineHost(N2kEngine engine, IHostWaiter? waiter = null)
    {
        _engine = engine;
        _waiter = waiter ?? DefaultWaiter.Instance;
        _thread = new Thread(Loop) { IsBackground = true, Name = "n2k-engine", Priority = ThreadPriority.AboveNormal };
        _thread.Start();
    }

    public Exception? Fault { get; private set; }

    private void Loop()
    {
        while (_run)
        {
            long next;
            try { next = _engine.Step(); }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                // Operations are isolated in Step; this is the engine itself. Note it and go on.
                Fault = ex;
                next = _engine.Clock.NowUs + 100_000;
            }
            if (!_run) break;
            long cap = _engine.Clock.NowUs + 1_000_000;
            try
            {
                _waiter.Wait(_engine.WakeHandle, Math.Min(next, cap), _engine.Clock);
            }
            catch (ObjectDisposedException)
            {
                // The host disposed the waiter (or the engine) first: this loop is over.
                // An exception escaping a background thread would end the process.
                break;
            }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                // A broken host waiter: note it and pace with the portable one.
                Fault = ex;
                try { DefaultWaiter.Instance.Wait(_engine.WakeHandle, Math.Min(next, cap), _engine.Clock); }
                catch (ObjectDisposedException) { break; }
            }
        }
    }

    public void Dispose()
    {
        _run = false;
        _engine.Post(() => { });
        _thread.Join(2000);
    }
}

/// <summary>
/// The async face of the sender library (12.2): discover displays, identify one,
/// update one. One client owns one adapter; operations run one after another on the
/// engine thread. Progress callbacks arrive on the engine thread unless the
/// <see cref="IProgress{T}"/> marshals them (Progress&lt;T&gt; created on a UI thread does).
/// </summary>
public sealed class N2kUpdateClient : IDisposable
{
    private readonly EngineHost _host;
    private readonly SemaphoreSlim _exclusive = new(1, 1);

    /// <param name="link">The CAN adapter; opened by <see cref="StartAsync"/>.</param>
    /// <param name="options">NAME, preferred address, product information, bus load.</param>
    /// <param name="waiter">Pacing waits (6.6); null = <see cref="DefaultWaiter"/>.</param>
    /// <param name="clock">Monotonic time; null = <see cref="SystemClock"/>.</param>
    /// <param name="sharedSource">
    /// An address claimed by the host itself on the same adapter (12.3, NauticPi in slcan
    /// mode); null lets the client claim its own (the standalone updater).
    /// </param>
    public N2kUpdateClient(ICanFrameLink link, EngineOptions options, IHostWaiter? waiter = null, IClock? clock = null,
                           ISourceAddress? sharedSource = null)
    {
        Engine = new N2kEngine(link, clock ?? SystemClock.Instance, options, sharedSource: sharedSource);
        Engine.Log += m => Log?.Invoke(m);
        Engine.BusSampled += s => BusSampled?.Invoke(s);
        Engine.LinkStateChanged += up => LinkStateChanged?.Invoke(up);
        _host = new EngineHost(Engine, waiter);
    }

    public N2kEngine Engine { get; }

    /// <summary>Diagnostic lines (engine thread).</summary>
    public event Action<string>? Log;

    /// <summary>One bus-load sample per second (engine thread).</summary>
    public event Action<BusLoadSample>? BusSampled;

    /// <summary>The adapter went away (false) or is back (true) (engine thread).</summary>
    public event Action<bool>? LinkStateChanged;

    public int SourceAddress => Engine.Source.Address;

    /// <summary>Opens the adapter and claims an address; completes when the claim stands (250 ms).</summary>
    public async Task StartAsync(CancellationToken ct = default)
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Engine.Post(() =>
        {
            try { Engine.Start(); started.TrySetResult(); }
            catch (Exception ex) { started.TrySetException(ex); }
        });
        await started.Task.WaitAsync(ct).ConfigureAwait(false);
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (!Engine.Source.IsUsable)
        {
            if (DateTime.UtcNow > deadline) throw new IOException("no source address could be claimed");
            await Task.Delay(20, ct).ConfigureAwait(false);
        }
    }

    public async Task<DiscoveryResult> DiscoverAsync(SessionTimers? timers = null, CancellationToken ct = default)
    {
        await _exclusive.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            var op = new DiscoveryOperation(Engine, timers);
            var tcs = new TaskCompletionSource<DiscoveryResult>(TaskCreationOptions.RunContinuationsAsynchronously);
            op.Completed += r => tcs.TrySetResult(r);
            Engine.Post(() => Engine.Add(op));
            using var reg = ct.Register(() => Engine.Post(() => op.Cancel(Engine.Clock.NowUs)));
            return await tcs.Task.ConfigureAwait(false);
        }
        finally { _exclusive.Release(); }
    }

    /// <summary>Sends IDENTIFY (3.11). False if the adapter is gone and could not be reopened.</summary>
    public async Task<bool> IdentifyAsync(DisplayDevice device, int seconds = 10, CancellationToken ct = default)
    {
        await _exclusive.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            var op = new IdentifyOperation(Engine, device, seconds);
            var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            op.Completed += sent => tcs.TrySetResult(sent);
            Engine.Post(() => Engine.Add(op));
            return await tcs.Task.WaitAsync(ct).ConfigureAwait(false);
        }
        finally { _exclusive.Release(); }
    }

    /// <summary>
    /// Firmware, then the companion web UI (12.2). Cancelling sends ABORT(SENDER_CANCELLED);
    /// during the validation wait it only stops the polls, and the display then rolls
    /// back (H5) — callers warn before they cancel there (12.7).
    /// </summary>
    public async Task<UpdateReport> UpdateAsync(DisplayDevice device, UpdateRequest request, UpdateOptions options,
                                                IProgress<UpdateProgress>? progress = null, CancellationToken ct = default)
    {
        await _exclusive.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            // Hashing and deflate happen here, off the engine thread. Not cancelled midway:
            // a cancel that arrives meanwhile reaches the job before its first tick
            // (posted after Add), and it then ends without sending anything.
            var job = await Task.Run(() => new UpdateJob(Engine, device, request, options,
                                                         progress is null ? null : progress.Report)).ConfigureAwait(false);
            foreach (var w in job.Warnings) Log?.Invoke("warning: " + w);
            var tcs = new TaskCompletionSource<UpdateReport>(TaskCreationOptions.RunContinuationsAsynchronously);
            job.Completed += r => tcs.TrySetResult(r);
            Engine.Post(() =>
            {
                Engine.SetBusLoad(options.BusLoad);
                Engine.Add(job);
            });
            using var reg = ct.Register(() => Engine.Post(() => job.Cancel(Engine.Clock.NowUs)));
            return await tcs.Task.ConfigureAwait(false);
        }
        finally { _exclusive.Release(); }
    }

    public void Dispose()
    {
        _host.Dispose();
        try { Engine.Link.Close(); } catch { /* best effort */ }
        _exclusive.Dispose();
    }
}
