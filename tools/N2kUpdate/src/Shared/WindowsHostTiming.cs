using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Microsoft.Win32.SafeHandles;

namespace NauticPinnace.N2kUpdate.Hosting;

/// <summary>
/// Windows host timing for the sender (6.6). Not part of the library, which must
/// stay free of Windows-only APIs; linked into the Windows updater, the CLI and the
/// tests, and only used when running on Windows.
///   - waits use a high-resolution waitable timer (CreateWaitableTimerExW with
///     CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, Windows 10 1803 or later); if that is
///     unavailable, timeBeginPeriod(1) is set for the lifetime of the waiter;
///   - KeepAwake() stops the laptop from sleeping during a session.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class WindowsHighResWaiter : IHostWaiter, IDisposable
{
    private const uint CreateWaitableTimerHighResolution = 0x00000002;
    private const uint TimerAllAccess = 0x1F0003;

    private readonly TimerHandle? _timer;
    private readonly bool _periodSet;

    public WindowsHighResWaiter()
    {
        IntPtr h = CreateWaitableTimerExW(IntPtr.Zero, null, CreateWaitableTimerHighResolution, TimerAllAccess);
        if (h != IntPtr.Zero)
        {
            _timer = new TimerHandle(h);
        }
        else
        {
            _periodSet = timeBeginPeriod(1) == 0;
        }
    }

    /// <summary>True if the high-resolution waitable timer is in use (else timeBeginPeriod(1)).</summary>
    public bool HighResolution => _timer is not null;

    public void Wait(WaitHandle wake, long untilUs, IClock clock)
    {
        long left = untilUs - clock.NowUs;
        if (left <= 0) return;
        if (_timer is null)
        {
            // timeBeginPeriod(1) makes the kernel wait accurate to about 1 ms.
            DefaultWaiter.Instance.Wait(wake, untilUs, clock);
            return;
        }
        // Relative due time in 100 ns units, negative = relative.
        long due = -Math.Max(1, left * 10);
        if (!SetWaitableTimer(_timer, ref due, 0, IntPtr.Zero, IntPtr.Zero, false))
        {
            DefaultWaiter.Instance.Wait(wake, untilUs, clock);
            return;
        }
        using var timerWait = new TimerWaitHandle(_timer);
        WaitHandle.WaitAny([wake, timerWait]);
    }

    public void Dispose()
    {
        _timer?.Dispose();
        if (_periodSet) timeEndPeriod(1);
    }

    /// <summary>
    /// Keeps the system awake until the returned object is disposed
    /// (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED), 6.6). Must be
    /// disposed on the thread that created it.
    /// </summary>
    public static IDisposable KeepAwake() => new AwakeScope();

    private sealed class AwakeScope : IDisposable
    {
        private const uint EsContinuous = 0x80000000, EsSystemRequired = 0x00000001;
        private bool _done;
        public AwakeScope() => SetThreadExecutionState(EsContinuous | EsSystemRequired);
        public void Dispose()
        {
            if (_done) return;
            _done = true;
            SetThreadExecutionState(EsContinuous);
        }
    }

    private sealed class TimerHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        public TimerHandle(IntPtr h) : base(true) => SetHandle(h);
        protected override bool ReleaseHandle() => CloseHandle(handle);
    }

    /// <summary>Wraps the timer so WaitAny can wait on it together with the wake event.</summary>
    private sealed class TimerWaitHandle : WaitHandle
    {
        public TimerWaitHandle(TimerHandle h) => SafeWaitHandle = new SafeWaitHandle(h.DangerousGetHandle(), ownsHandle: false);
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWaitableTimerExW(IntPtr attributes, string? name, uint flags, uint access);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWaitableTimer(TimerHandle timer, ref long dueTime, int period, IntPtr completion, IntPtr arg, [MarshalAs(UnmanagedType.Bool)] bool resume);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll")]
    private static extern uint SetThreadExecutionState(uint flags);

    [DllImport("winmm.dll")]
    private static extern uint timeBeginPeriod(uint period);

    [DllImport("winmm.dll")]
    private static extern uint timeEndPeriod(uint period);
}
