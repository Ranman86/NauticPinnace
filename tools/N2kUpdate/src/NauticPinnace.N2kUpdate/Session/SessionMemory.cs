using System.Globalization;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Session;

/// <summary>
/// The sessions this sender opened, one per display (unique id). A sender claims RESUME
/// (5.3.4) only for a session it can show is its own: the display's INFO names the session
/// it holds, and that id must be one this sender opened for exactly this stream. Otherwise
/// a second sender offering the same release would take a running transfer away from the
/// first (5.8 "Two senders").
///
/// Kept in memory per engine, and with a cache directory also on disk (sessions.txt),
/// next to the deflate cache that keeps the stream byte-identical (12.4) - so a sender
/// that crashed or was restarted can resume its own session, and one without that
/// directory offers anew (5.3.5 then replaces its stale session after 5 s of silence).
/// Engine thread only.
/// </summary>
public sealed class SessionMemory
{
    /// <summary>A record older than this no longer counts (the display keeps a session 60 s
    /// after the sender's last message, a finished one at most 10 min + 30 s).</summary>
    public static readonly TimeSpan MaxAge = TimeSpan.FromMinutes(15);

    private const string FileName = "sessions.txt";

    public sealed record Entry(UniqueId Display, byte SessionId, UpdateTarget Target, StreamEncoding Encoding,
                               string Sha256Hex, uint StreamCrc32, uint StreamSize, DateTime Utc);

    private readonly Dictionary<UniqueId, Entry> _mem = new();

    public static Entry For(UniqueId display, byte sessionId, TransferImage img) =>
        new(display, sessionId, img.Target, img.Encoding, Convert.ToHexString(img.Sha256), img.StreamCrc32,
            img.StreamSize, DateTime.UtcNow);

    /// <summary>The session <paramref name="e"/> was opened (the display answered its OFFER).</summary>
    public void Remember(string? dir, Entry e)
    {
        _mem[e.Display] = e;
        if (dir is null) return;
        var all = Load(dir);
        all[e.Display] = e;
        Save(dir, all);
    }

    /// <summary>The session ended: it is nothing to resume any more.</summary>
    public void Forget(string? dir, UniqueId display, byte sessionId)
    {
        if (_mem.TryGetValue(display, out var m) && m.SessionId == sessionId) _mem.Remove(display);
        if (dir is null) return;
        var all = Load(dir);
        if (all.TryGetValue(display, out var d) && d.SessionId == sessionId)
        {
            all.Remove(display);
            Save(dir, all);
        }
    }

    /// <summary>
    /// True if the session <paramref name="heldSessionId"/> that the display reports is one
    /// this sender opened for exactly this stream, recently enough to resume it.
    /// </summary>
    public bool IsOwn(string? dir, UniqueId display, byte heldSessionId, TransferImage img)
    {
        Entry? e = _mem.GetValueOrDefault(display);
        if (e is null && dir is not null) e = Load(dir).GetValueOrDefault(display);
        return e is not null && e.SessionId == heldSessionId && e.Target == img.Target && e.Encoding == img.Encoding &&
               e.StreamCrc32 == img.StreamCrc32 && e.StreamSize == img.StreamSize &&
               e.Sha256Hex == Convert.ToHexString(img.Sha256) && DateTime.UtcNow - e.Utc < MaxAge;
    }

    // One line per display: uid session target encoding sha256 crc size utc-ticks.
    private static Dictionary<UniqueId, Entry> Load(string dir)
    {
        var d = new Dictionary<UniqueId, Entry>();
        string path = Path.Combine(dir, FileName);
        try
        {
            if (!File.Exists(path)) return d;
            foreach (string line in File.ReadAllLines(path))
            {
                string[] f = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (f.Length != 8) continue;
                if (!ulong.TryParse(f[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out ulong uid) ||
                    !byte.TryParse(f[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out byte sid) ||
                    !byte.TryParse(f[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out byte target) ||
                    !byte.TryParse(f[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out byte enc) ||
                    f[4].Length != 64 ||
                    !uint.TryParse(f[5], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint crc) ||
                    !uint.TryParse(f[6], NumberStyles.Integer, CultureInfo.InvariantCulture, out uint size) ||
                    !long.TryParse(f[7], NumberStyles.Integer, CultureInfo.InvariantCulture, out long ticks) ||
                    ticks < DateTime.MinValue.Ticks || ticks > DateTime.MaxValue.Ticks)
                    continue;
                var id = new UniqueId(uid);
                d[id] = new Entry(id, sid, (UpdateTarget)target, (StreamEncoding)enc, f[4].ToUpperInvariant(), crc, size,
                                  new DateTime(ticks, DateTimeKind.Utc));
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { /* a convenience only */ }
        return d;
    }

    private static void Save(string dir, Dictionary<UniqueId, Entry> all)
    {
        try
        {
            Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, FileName), tmp = path + ".tmp";
            File.WriteAllLines(tmp, all.Values
                .Where(e => DateTime.UtcNow - e.Utc < MaxAge)
                .Select(e => string.Create(CultureInfo.InvariantCulture,
                    $"{e.Display.Value:X12} {e.SessionId} {(byte)e.Target} {(byte)e.Encoding} {e.Sha256Hex} {e.StreamCrc32:X8} {e.StreamSize} {e.Utc.Ticks}")));
            File.Move(tmp, path, overwrite: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { /* a convenience only */ }
    }
}
