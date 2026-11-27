using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Tests;

/// <summary>Encode/decode round trips (section 3) and the fast-packet layer (2.3, 12.2).</summary>
public static class CodecTests
{
    private static IEnumerable<UpdateMessage> AllMessages()
    {
        yield return new QueryMsg { SessionId = 7, What = QueryWhat.Ack, UidFilter = new UniqueId(0x0A0B0C0D0E0F) };
        yield return VectorTests.SampleInfo() with { LastInvalidVersion = "1.2.9-rc1", RollbackCause = RollbackCause.HealthCheck, RollbackDetail = 5, LastInvalidImageId = new ImageId(42) };
        yield return VectorTests.SampleOffer() with { Flags = OfferFlags.Resume | OfferFlags.AllowVersionMismatch };
        yield return new OfferReplyMsg { SessionId = 9, Decision = OfferDecision.Rejected, Reason = Reason.WrongBoard };
        yield return new AckMsg { SessionId = 3, State = SessionState.Receiving, Base = 216 * 64, End = 216 * 128, Bitmap = 0x8000_0000_0000_0001, StreamCrc32 = 0xDEADBEEF };
        yield return new ResultMsg { SessionId = 4, Code = Reason.ApplyFailed, Phase = ResultPhase.Activated, Detail = 1, Sha256 = Enumerable.Range(0, 32).Select(i => (byte)i).ToArray() };
        yield return new ApplyMsg { SessionId = 5, Mode = ApplyWireMode.NowNoWait };
        yield return new AbortMsg { SessionId = 6, Reason = Reason.LinkUnreliable };
        yield return new StatusMsg { SessionId = 8, State = SessionState.Processing, Reason = Reason.Ok, StreamAccepted = 1, StreamSize = 2, ImageWritten = 3, ImageSize = 4, Seconds = 5 };
        yield return new IdentifyMsg { SessionId = 0, Seconds = 30, TargetUid = new UniqueId(0x112233445566) };
    }

    [Test]
    public static void RoundTrips()
    {
        foreach (var m in AllMessages())
        {
            byte[] a = m.Encode();
            Assert.Equal(UpdProto.MinLength(m.Type), a.Length, $"{m.Type} v1 length");
            var back = UpdateMessage.Decode(a) ?? throw new TestFailure($"{m.Type} did not decode");
            Assert.Equal(m.Type, back.Type, "type");
            Assert.Hex(Convert.ToHexString(a), back.Encode(), $"{m.Type} re-encode");
        }
        var data = new DataMsg { SessionId = 0x5A, Offset = 216 * 100, Data = new byte[] { 1, 2, 3 } };
        var d = (DataMsg)UpdateMessage.Decode(data.Encode())!;
        Assert.Equal(216u * 100, d.Offset, "DATA offset");
        Assert.Hex("010203", d.Data.Span, "final-chunk data");
    }

    [Test]
    public static void DecodeRejectsForeignAndShort()
    {
        byte[] info = VectorTests.SampleInfo().Encode();
        var media = (byte[])info.Clone();
        media[0] = 0xA3; media[1] = 0x99;   // manufacturer 419: not this protocol
        Assert.True(UpdateMessage.Decode(media) is null, "other manufacturer header ignored");
        var unknown = (byte[])info.Clone();
        unknown[2] = 0xAB;
        Assert.True(UpdateMessage.Decode(unknown) is null, "unknown type ignored");
        unknown[2] = 0x9F;
        Assert.True(UpdateMessage.Decode(unknown) is null, "type below range ignored");
        Assert.True(UpdateMessage.Decode(info.AsSpan(0, 128)) is null, "INFO shorter than 129 ignored");
        Assert.True(UpdateMessage.Decode(new AckMsg { SessionId = 1 }.Encode().AsSpan(0, 27)) is null, "ACK shorter than 28 ignored");
        Assert.True(UpdateMessage.Decode(DataMsg.Build(1, 0, ReadOnlySpan<byte>.Empty)) is null, "DATA without bytes ignored");
    }

    [Test]
    public static void DecodeAcceptsLongerMessages()
    {
        // 3.0: a later minor revision may append fields; receivers ignore them.
        byte[] info = VectorTests.SampleInfo().Encode();
        var longer = info.Concat(new byte[] { 1, 2, 3, 4 }).ToArray();
        var m = UpdateMessage.Decode(longer) as InfoMsg ?? throw new TestFailure("longer INFO rejected");
        Assert.Hex(Convert.ToHexString(info), m.Encode(), "fields unchanged");
    }

    [Test]
    public static void TextFields()
    {
        Assert.True(TextField.IsValidVersion("1.3.0"), "1.3.0");
        Assert.True(TextField.IsValidVersion("123456789012345"), "15 characters");
        Assert.True(!TextField.IsValidVersion("1234567890123456"), "16 characters are too many");
        Assert.True(!TextField.IsValidVersion(""), "empty");
        Assert.True(!TextField.IsValidVersion("1.3ä"), "non-ASCII");
        // UTF-8 cut at a character boundary: 15 ASCII + 'ä' (2 bytes) must drop the 'ä'.
        var b = new byte[16];
        TextField.WriteUtf8(b, "ABCDEFGHIJKLMNOä");
        Assert.Equal("ABCDEFGHIJKLMNO", TextField.Read(b), "cut at a character boundary");
        TextField.WriteUtf8(b, "Bootsname 16 Byt");
        Assert.Equal("Bootsname 16 Byt", TextField.Read(b), "full field without NUL");
    }

    [Test]
    public static void FastPacketFraming()
    {
        uint id = 0x19EF2AE6;
        var one = FastPacket.Encode(id, new byte[] { 0xAA }, 0);
        Assert.Equal(1, one.Count, "1 byte: one frame");
        Assert.Hex("00 01 AA FF FF FF FF FF", one[0].Data, "1 byte framed, not raw (2.3 rule 1)");
        var six = FastPacket.Encode(id, new byte[] { 1, 2, 3, 4, 5, 6 }, 3);
        Assert.Equal(1, six.Count, "6 bytes: one frame");
        Assert.Hex("60 06 01 02 03 04 05 06", six[0].Data, "6 bytes, seq 3");
        var seven = FastPacket.Encode(id, new byte[] { 1, 2, 3, 4, 5, 6, 7 }, 7);
        Assert.Equal(2, seven.Count, "7 bytes: two frames");
        Assert.Hex("E0 07 01 02 03 04 05 06", seven[0].Data, "7 bytes frame 0");
        Assert.Hex("E1 07 FF FF FF FF FF FF", seven[1].Data, "7 bytes frame 1");
        var full = FastPacket.Encode(id, new byte[223], 2);
        Assert.Equal(32, full.Count, "223 bytes: 32 frames");
        Assert.Equal(0x5F, full[31].Data[0], "last frame index 31 with seq 2");
        Assert.True(full.All(f => f.Length == 8), "DLC always 8");

        var seq = new FastPacketSequencer();
        var seen = Enumerable.Range(0, 10).Select(_ => seq.Take(0xE6, UpdProto.Pgn)).ToArray();
        Assert.Equal("0,1,2,3,4,5,6,7,0,1", string.Join(",", seen), "seq wraps after 7");
        Assert.Equal(0, seq.Take(0xE6, 126996), "separate counter per PGN");
    }

    [Test]
    public static void Reassembly()
    {
        var asm = new FastPacketAssembler();
        byte[] payload = Enumerable.Range(0, 40).Select(i => (byte)i).ToArray();
        var fr = FastPacket.Encode(0x19EFE62A, payload, 1);

        // In order, with a 1.9 s gap (4-inch TX stall, 2.4).
        byte[]? r = null;
        long t = 0;
        foreach (var f in fr) { r = asm.Add(0x2A, 0xE6, UpdProto.Pgn, f.Data, t); t += 1_900_000; }
        Assert.Hex(Convert.ToHexString(payload), r ?? throw new TestFailure("1.9 s gaps were not accepted"), "reassembled");

        // A gap above 2 s drops the message.
        t = 0; r = null;
        for (int i = 0; i < fr.Count; i++) { r = asm.Add(0x2A, 0xE6, UpdProto.Pgn, fr[i].Data, t); t += i == 2 ? 2_100_000 : 1000; }
        Assert.True(r is null, "gap of 2.1 s dropped the message");

        // A missing frame drops the message (no partial delivery).
        r = null;
        for (int i = 0; i < fr.Count; i++) if (i != 3) r = asm.Add(0x2A, 0xE6, UpdProto.Pgn, fr[i].Data, 0) ?? r;
        Assert.True(r is null, "missing frame dropped the message");

        // A new frame 0 for the same key restarts.
        asm.Add(0x2A, 0xE6, UpdProto.Pgn, fr[0].Data, 0);
        asm.Add(0x2A, 0xE6, UpdProto.Pgn, fr[1].Data, 0);
        r = null;
        foreach (var f in fr) r = asm.Add(0x2A, 0xE6, UpdProto.Pgn, f.Data, 0) ?? r;
        Assert.Hex(Convert.ToHexString(payload), r ?? throw new TestFailure("restart failed"), "restarted message");

        // Different destinations are separate keys: interleaving does not mix them.
        var other = FastPacket.Encode(0x19EF992A, payload.Reverse().ToArray(), 1);
        byte[]? a = null, b = null;
        for (int i = 0; i < fr.Count; i++)
        {
            a = asm.Add(0x2A, 0xE6, UpdProto.Pgn, fr[i].Data, 0) ?? a;
            b = asm.Add(0x2A, 0x99, UpdProto.Pgn, other[i].Data, 0) ?? b;
        }
        Assert.Hex(Convert.ToHexString(payload), a!, "message to us");
        Assert.Hex(Convert.ToHexString(payload.Reverse().ToArray()), b!, "message to 0x99");
    }

    [Test]
    public static void SlcanLines()
    {
        var f = new CanFrame(0x1DEF2AE6, [0x00, 0xDF, 0xFE, 0x9F, 0xA4, 0x5A, 0x00, 0x00]);
        var buf = new byte[32];
        int n = SlcanFrameLink.Format(f, buf);
        Assert.Equal(27, n, "27 bytes per frame (6.4)");
        Assert.Equal("T1DEF2AE6800DFFE9FA45A0000\r", System.Text.Encoding.ASCII.GetString(buf, 0, n), "slcan line");
        Assert.True(SlcanFrameLink.TryParse("T1DEF2AE6800DFFE9FA45A0000", out var back), "parse");
        Assert.Equal(f.Id, back.Id, "id");
        Assert.Hex(Convert.ToHexString(f.Data), back.Data, "data");
        Assert.True(SlcanFrameLink.TryParse("T19EFE62A3010203ABCD", out var withTs) && withTs.Length == 3, "timestamp suffix ignored");
        Assert.True(!SlcanFrameLink.TryParse("t1238AABB", out _), "11-bit frames are not ours");
    }

    [Test]
    public static void CanLogRoundTrip()
    {
        string path = Path.Combine(Path.GetTempPath(), $"n2kupd-{Guid.NewGuid():N}.canlog");
        try
        {
            using (var w = new CanLogWriter(path))
            {
                w.Write(1_234_567, new CanFrame(0x1DEF2AE6, [0, 0xDF, 0xFE, 0x9F, 0xA4, 0x5A, 0, 0]));
                w.Write(2_000_000, new CanFrame(0x18EEFFE6, [1, 2, 3, 4, 5, 6, 7, 8]));
            }
            var lines = File.ReadAllLines(path);
            Assert.Equal("1234 1DEF2AE6 8 00DFFE9FA45A0000", lines[0], "line format t_ms id29 dlc hex");
            var back = CanLogReader.Read(path).ToList();
            Assert.Equal(2, back.Count, "two frames");
            Assert.Equal(2000L, back[1].TimeMs, "time");
            Assert.Equal(0x18EEFFE6u, back[1].Frame.Id, "id");
        }
        finally { File.Delete(path); }
    }
}
