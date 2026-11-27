using System.Diagnostics;

namespace NauticPinnace.N2kUpdate;

/// <summary>
/// The monotonic clock of the sender engine, in microseconds. Real runs use
/// <see cref="SystemClock"/> (Stopwatch, 6.6); tests drive a virtual clock.
/// </summary>
public interface IClock
{
    long NowUs { get; }
}

/// <summary>Monotonic wall time from <see cref="Stopwatch.GetTimestamp"/> (6.6).</summary>
public sealed class SystemClock : IClock
{
    public static readonly SystemClock Instance = new();
    private static readonly long T0 = Stopwatch.GetTimestamp();
    private static readonly double UsPerTick = 1_000_000.0 / Stopwatch.Frequency;

    public long NowUs => (long)((Stopwatch.GetTimestamp() - T0) * UsPerTick);
}

/// <summary>
/// Waits of the engine thread: until a deadline or until the wake handle is
/// signalled, whichever comes first. Pacing depends on how precisely this returns
/// (6.6): on Windows the host injects a high-resolution waitable timer, because the
/// default timer granularity of 15.6 ms would make transfers up to 20 % slower.
/// </summary>
public interface IHostWaiter
{
    void Wait(WaitHandle wake, long untilUs, IClock clock);
}

/// <summary>
/// Portable waiter: a kernel wait for the bulk of the time, then a short spin for
/// the last fraction of a millisecond. Precise on Linux (about 1 ms timer
/// resolution, enough per 6.6). On Windows, without a host-injected waiter, the
/// kernel wait may overshoot; the token bucket then only sends later, never above
/// the configured share.
/// </summary>
public sealed class DefaultWaiter : IHostWaiter
{
    public static readonly DefaultWaiter Instance = new();

    /// <summary>Remaining time below which the waiter spins instead of sleeping.</summary>
    public long SpinBelowUs { get; init; } = 1_500;

    public void Wait(WaitHandle wake, long untilUs, IClock clock)
    {
        while (true)
        {
            long left = untilUs - clock.NowUs;
            if (left <= 0) return;
            if (left > SpinBelowUs)
            {
                int ms = (int)Math.Min(int.MaxValue, (left - SpinBelowUs) / 1000);
                if (wake.WaitOne(Math.Max(1, ms))) return;
                continue;
            }
            if (wake.WaitOne(0)) return;
            Thread.SpinWait(50);
        }
    }
}
