namespace NauticPinnace.N2kUpdate.N2k;

public enum ClaimState { Idle, Claiming, Claimed, Failed }

/// <summary>The claimed source address plus a "changed" event (12.2, ISourceAddress).</summary>
public interface ISourceAddress
{
    /// <summary>The address in use, or 254 while none is usable.</summary>
    int Address { get; }

    /// <summary>True once the claim has stood for 250 ms.</summary>
    bool IsUsable { get; }

    /// <summary>Raised with (old, new) once a new address is usable.</summary>
    event Action<int, int>? Changed;
}

/// <summary>
/// ISO address claim for the sender (12.3, J1939-81): claim with PGN 60928, wait
/// 250 ms, defend the address against a NAME of lower priority, move to the next
/// free address after losing it to a NAME of higher priority (lower value), and
/// answer ISO requests for 60928. Pure state machine; the engine sends what
/// <see cref="Outbox"/> collects.
/// </summary>
public sealed class AddressClaimer : ISourceAddress
{
    public const long ClaimWaitUs = 250_000;

    private readonly Dictionary<int, N2kName> _inUse = new();
    private long _claimSince;
    private int _usable = N2kPgn.NullAddress;
    private int _lastUsable = N2kPgn.NullAddress;

    public AddressClaimer(N2kName name, int preferredAddress)
    {
        if (preferredAddress < 0 || preferredAddress > 251)
            throw new ArgumentOutOfRangeException(nameof(preferredAddress));
        Name = name;
        PreferredAddress = preferredAddress;
    }

    public N2kName Name { get; }
    public int PreferredAddress { get; }
    public ClaimState State { get; private set; } = ClaimState.Idle;

    /// <summary>The address being claimed or held (254 when none).</summary>
    public int Candidate { get; private set; } = N2kPgn.NullAddress;

    public int Address => _usable;
    public bool IsUsable => State == ClaimState.Claimed;

    public event Action<int, int>? Changed;

    /// <summary>Address claims to send: (source address, NAME). Drained by the engine.</summary>
    public Queue<(int Source, N2kName Name)> Outbox { get; } = new();

    /// <summary>Nodes seen on the bus with their NAME (from their address claims).</summary>
    public IReadOnlyDictionary<int, N2kName> KnownNodes => _inUse;

    public void Start(long nowUs)
    {
        // An address known to be in use is skipped rather than contested: winning it
        // by NAME priority would push an instrument off its address.
        BeginClaim(PickFree(PreferredAddress, includeStart: true), nowUs);
    }

    public long Tick(long nowUs)
    {
        if (State == ClaimState.Claiming)
        {
            long due = _claimSince + ClaimWaitUs;
            if (nowUs < due) return due;
            State = ClaimState.Claimed;
            _usable = Candidate;
            int old = _lastUsable;
            _lastUsable = _usable;
            if (old != _usable) Changed?.Invoke(old, _usable);
        }
        return long.MaxValue;
    }

    /// <summary>A received address claim (PGN 60928) from <paramref name="source"/>.</summary>
    public void OnClaim(int source, N2kName other, long nowUs)
    {
        if (other == Name) return;           // our own claim, echoed by a bridge
        if (source == N2kPgn.NullAddress) return;   // "cannot claim" of someone else
        _inUse[source] = other;
        if (State is ClaimState.Idle or ClaimState.Failed || source != Candidate) return;

        if (other.Value < Name.Value)
        {
            // Lost: the other NAME has priority. Move on.
            int next = PickFree(Candidate, includeStart: false);
            if (next == N2kPgn.NullAddress)
            {
                State = ClaimState.Failed;
                Candidate = N2kPgn.NullAddress;
                SetUnusable();
                Outbox.Enqueue((N2kPgn.NullAddress, Name));
                return;
            }
            SetUnusable();
            BeginClaim(next, nowUs);
        }
        else
        {
            // Won: defend by claiming again.
            Outbox.Enqueue((Candidate, Name));
        }
    }

    /// <summary>A source address seen in any frame; helps to pick a free address.</summary>
    public void NoteSource(int source)
    {
        if (source < 252 && source != Candidate && !_inUse.ContainsKey(source))
            _inUse[source] = default;
    }

    /// <summary>An ISO request for PGN 60928 addressed to us or to 255.</summary>
    public void OnClaimRequest()
    {
        if (State is ClaimState.Claiming or ClaimState.Claimed) Outbox.Enqueue((Candidate, Name));
        else if (State == ClaimState.Failed) Outbox.Enqueue((N2kPgn.NullAddress, Name));
    }

    private void BeginClaim(int address, long nowUs)
    {
        Candidate = address;
        State = ClaimState.Claiming;
        _claimSince = nowUs;
        Outbox.Enqueue((address, Name));
    }

    private void SetUnusable()
    {
        if (_usable == N2kPgn.NullAddress) return;
        _usable = N2kPgn.NullAddress;
    }

    private int PickFree(int start, bool includeStart)
    {
        for (int i = includeStart ? 0 : 1; i < 252; i++)
        {
            int a = (start + i) % 252;
            if (!_inUse.ContainsKey(a)) return a;
        }
        return N2kPgn.NullAddress;
    }
}
