using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Tests;

/// <summary>
/// Appendix A of docs/N2K-UPDATE.md, byte for byte: sender 0xE6, display 0x2A,
/// session 0x5A, unique id 02 00 00 00 00 01, fast-packet seq 0 unless stated.
/// The per-frame bit counts not quoted in the appendix come from vectors.py
/// (vectors_out.txt), the generator the appendix was made with.
/// </summary>
public static class VectorTests
{
    private const int Snd = 0xE6, Dev = 0x2A;
    private const byte Sess = 0x5A;
    private static readonly UniqueId Uid = new(0x020000000001);
    private static readonly byte[] ImgSha = SHA256.HashData(Encoding.ASCII.GetBytes("NauticPinnace N2K update test image"));
    private static readonly byte[] WebSha = SHA256.HashData(Encoding.ASCII.GetBytes("NauticPinnace N2K update test web UI"));

    private static uint IdCtl(int src, int dst) => CanId.Encode(6, UpdProto.Pgn, src, dst);

    /// <summary>Checks a message's frames: hex per frame and, where given, the exact on-wire bits.</summary>
    private static void Frames(uint id, byte[] payload, int seq, string[] frames, int?[]? bits = null, string what = "")
    {
        var fr = FastPacket.Encode(id, payload, seq);
        Assert.Equal(frames.Length, fr.Count, what + " frame count");
        for (int i = 0; i < frames.Length; i++)
        {
            if (frames[i].Length == 0) continue;
            Assert.Hex(frames[i], fr[i].Data, $"{what} frame {i}");
            Assert.Equal(8, fr[i].Length, $"{what} frame {i} DLC");
            if (bits is not null && i < bits.Length && bits[i] is int b)
                Assert.Equal(b, FrameBits.Exact(fr[i]), $"{what} frame {i} bits");
        }
    }

    [Test]
    public static void CanIds()
    {
        Assert.Equal(0x1DEF2AE6u, CanId.Encode(7, UpdProto.Pgn, Snd, Dev), "DATA id");
        Assert.Equal(0x19EFE62Au, CanId.Encode(6, UpdProto.Pgn, Dev, Snd), "ACK id");
        Assert.Equal(0x19EFFFE6u, CanId.Encode(6, UpdProto.Pgn, Snd, 0xFF), "broadcast QUERY id");
        var d = CanId.Decode(0x1DEF2AE6);
        Assert.Equal((UpdProto.Pgn, 7, Snd, Dev), d, "decode");
    }

    [Test]
    public static void Checksums()
    {
        Assert.Equal(0xCBF43926u, Crc32.Compute("123456789"u8), "crc32 check value");
        Assert.Equal(0xD822AEE5u, Crc32.Compute(Encoding.ASCII.GetBytes("NauticPinnace N2K update test stream")), "crc32 test stream");
        Assert.Hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", SHA256.HashData("abc"u8), "sha256(abc)");
        Assert.Hex("ee2f7fe70fd134410b3e0cdb41ab19309035da79696851fac2ac925cc26262cb", ImgSha, "sha256 test image");
        Assert.Hex("37558a1a25d09b4d7e46a5ec4ab40666eb9c65f9971659dd34e20ef8d8df03ab", WebSha, "sha256 test web UI");
        var stream = new byte[UpdProto.ChunkSize * UpdProto.MaxWindowChunks * 2];
        for (int i = 0; i < stream.Length; i++) stream[i] = (byte)i;
        Assert.Equal(0xF862F03Bu, Crc32.Compute(stream.AsSpan(0, 13824)), "crc32(stream[0:13824])");
        // Chaining like zlib.
        Assert.Equal(Crc32.Compute(stream), Crc32.Update(Crc32.Compute(stream.AsSpan(0, 1000)), stream.AsSpan(1000)), "chained crc");
    }

    [Test]
    public static void QueryInfoBroadcast()
    {
        var q = new QueryMsg { SessionId = 0, What = QueryWhat.Info };
        Assert.Hex("FE 9F A0 01 00 01 FF FF FF FF FF FF", q.Encode(), "payload");
        Frames(0x19EFFFE6, q.Encode(), 0, ["00 0C FE 9F A0 01 00 01", "01 FF FF FF FF FF FF FF"], [144, 148], "QUERY(INFO)");
    }

    [Test]
    public static void QueryInfoWithUid()
    {
        var q = new QueryMsg { SessionId = 0, What = QueryWhat.Info, UidFilter = Uid };
        Assert.Hex("FE 9F A0 01 00 01 02 00 00 00 00 01", q.Encode(), "payload");
        Frames(0x19EFFFE6, q.Encode(), 0, ["00 0C FE 9F A0 01 00 01", "01 02 00 00 00 00 01 FF"], [144, 149], "QUERY(INFO, uid)");
    }

    [Test]
    public static void QueryResult()
    {
        var q = new QueryMsg { SessionId = Sess, What = QueryWhat.Result };
        Assert.Hex("FE 9F A0 01 5A 04 FF FF FF FF FF FF", q.Encode(), "payload");
        Assert.Equal(0x19EF2AE6u, IdCtl(Snd, Dev), "id");
        Frames(0x19EF2AE6, q.Encode(), 0, ["00 0C FE 9F A0 01 5A 04", "01 FF FF FF FF FF FF FF"], [140, 147], "QUERY(RESULT)");
    }

    public static InfoMsg SampleInfo() => new()
    {
        SessionId = 0, Board = BoardId.B7, Caps = (Capabilities)0x3F, Uid = Uid,
        SessionState = SessionState.Idle, OtaState = OtaState.Valid, VerifySecondsLeft = 0xFFFF,
        RunningSlot = 0, Flags = (StatusFlags)0x11, MaxFirmwareSize = 0x4EF000, MaxWebUiSize = 0x20000,
        RunningVersion = "1.3.0", LastInvalidVersion = "", Label = "S/V Example",
        RunningImageId = new ImageId(0x1122334455667788), LastInvalidImageId = ImageId.None,
        WebUiVersion = "1.3.0", WebUiId = ImageId.Read(WebSha), WebUiSize = 48_751, UptimeSeconds = 754,
    };

    [Test]
    public static void Info()
    {
        var p = SampleInfo().Encode();
        Assert.Hex("""
            FE 9F A1 01 00 01 01 02 3F 02 00 00 00 00 01 00 01 FF FF 00 11 00 F0 4E
            00 00 00 02 00 D8 40 31 2E 33 2E 30 00 00 00 00 00 00 00 00 00 00 00 00
            00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 53 2F 56 20 45 78 61 6D 70
            6C 65 00 00 00 00 00 11 22 33 44 55 66 77 88 00 00 00 00 00 00 00 00 00
            00 31 2E 33 2E 30 00 00 00 00 00 00 00 00 00 00 00 37 55 8A 1A 25 D0 9B
            4D 6F BE 00 00 F2 02 00 00
            """, p, "INFO payload");
        Frames(0x19EFE62A, p, 0,
        [
            "00 81 FE 9F A1 01 00 01", "01 01 02 3F 02 00 00 00", "02 00 01 00 01 FF FF 00", "03 11 00 F0 4E 00 00 00",
            "04 02 00 D8 40 31 2E 33", "05 2E 30 00 00 00 00 00", "06 00 00 00 00 00 00 00", "07 00 00 00 00 00 00 00",
            "08 00 00 00 00 00 00 00", "09 00 53 2F 56 20 45 78", "0A 61 6D 70 6C 65 00 00", "0B 00 00 00 11 22 33 44",
            "0C 55 66 77 88 00 00 00", "0D 00 00 00 00 00 00 00", "0E 31 2E 33 2E 30 00 00", "0F 00 00 00 00 00 00 00",
            "10 00 00 37 55 8A 1A 25", "11 D0 9B 4D 6F BE 00 00", "12 F2 02 00 00 FF FF FF",
        ],
        [142, 144, 143, 142, 137, 142, 145, 145, 145, 135, 138, 139, 139, 145, 138, 144, 138, 139, 142], "INFO");
    }

    public static OfferMsg SampleOffer() => new()
    {
        SessionId = Sess, Target = UpdateTarget.Firmware, Board = BoardId.B7, Encoding = StreamEncoding.Deflate,
        Flags = OfferFlags.None, ImageSize = 2_441_792, StreamSize = 1_313_452, Sha256 = ImgSha, TargetUid = Uid,
        Version = "1.3.0", SenderName = "PC Updater", SenderKind = SenderKind.StandaloneUpdater, EstimatedSeconds = 360,
        StreamCrc32 = 0xD822AEE5, CompanionSha256 = WebSha, CompanionSize = 48_751,
    };

    [Test]
    public static void Offer()
    {
        var p = SampleOffer().Encode();
        Assert.Hex("""
            FE 9F A2 01 5A 01 02 01 00 D8 40 42 25 00 AC 0A 14 00 EE 2F 7F E7 0F D1
            34 41 0B 3E 0C DB 41 AB 19 30 90 35 DA 79 69 68 51 FA C2 AC 92 5C C2 62
            62 CB 02 00 00 00 00 01 31 2E 33 2E 30 00 00 00 00 00 00 00 00 00 00 00
            50 43 20 55 70 64 61 74 65 72 00 00 00 00 00 00 02 68 01 E5 AE 22 D8 37
            55 8A 1A 25 D0 9B 4D 7E 46 A5 EC 4A B4 06 66 EB 9C 65 F9 97 16 59 DD 34
            E2 0E F8 D8 DF 03 AB 6F BE 00 00
            """, p, "OFFER payload");
        Frames(0x19EF2AE6, p, 0,
        [
            "00 83 FE 9F A2 01 5A 01", "01 02 01 00 D8 40 42 25", "02 00 AC 0A 14 00 EE 2F", "03 7F E7 0F D1 34 41 0B",
            "04 3E 0C DB 41 AB 19 30", "05 90 35 DA 79 69 68 51", "06 FA C2 AC 92 5C C2 62", "07 62 CB 02 00 00 00 00",
            "08 01 31 2E 33 2E 30 00", "09 00 00 00 00 00 00 00", "0A 00 00 00 50 43 20 55", "0B 70 64 61 74 65 72 00",
            "0C 00 00 00 00 00 02 68", "0D 01 E5 AE 22 D8 37 55", "0E 8A 1A 25 D0 9B 4D 7E", "0F 46 A5 EC 4A B4 06 66",
            "10 EB 9C 65 F9 97 16 59", "11 DD 34 E2 0E F8 D8 DF", "12 03 AB 6F BE 00 00 FF",
        ],
        [140, 138, 138, 137, 137, 134, 135, 140, 137, 144, 140, 136, 143, 136, 134, 134, 134, 137, 142], "OFFER");
    }

    [Test]
    public static void OfferReplies()
    {
        var pend = new OfferReplyMsg { SessionId = Sess, Decision = OfferDecision.PendingUser, Seconds = 60 };
        Assert.Hex("FE 9F A3 01 5A 00 00 3C 00 00", pend.Encode(), "pending payload");
        Frames(0x19EFE62A, pend.Encode(), 0, ["00 0A FE 9F A3 01 5A 00", "01 00 3C 00 00 FF FF FF"], [139, 145], "pending");
        var acc = new OfferReplyMsg { SessionId = Sess, Decision = OfferDecision.Accepted, WindowChunks = 64 };
        Assert.Hex("FE 9F A3 01 5A 01 00 00 00 40", acc.Encode(), "accepted payload");
        Frames(0x19EFE62A, acc.Encode(), 0, ["00 0A FE 9F A3 01 5A 01", "01 00 00 00 40 FF FF FF"], [139, 145], "accepted");
    }

    [Test]
    public static void Data()
    {
        var stream = new byte[UpdProto.ChunkSize * 2];
        for (int i = 0; i < stream.Length; i++) stream[i] = (byte)i;
        var d0 = DataMsg.Build(Sess, 0, stream.AsSpan(0, 216));
        Assert.Equal(223, d0.Length, "full DATA length");
        var f0 = FastPacket.Encode(0x1DEF2AE6, d0, 0);
        Assert.Equal(32, f0.Count, "DATA frames");
        Assert.Hex("00 DF FE 9F A4 5A 00 00", f0[0].Data, "chunk 0 frame 0");
        Assert.Hex("01 00 00 01 02 03 04 05", f0[1].Data, "chunk 0 frame 1");
        Assert.Hex("1F D1 D2 D3 D4 D5 D6 D7", f0[31].Data, "chunk 0 frame 31");
        Assert.Equal(140, FrameBits.Exact(f0[0]), "chunk 0 frame 0 bits");
        Assert.Equal(143, FrameBits.Exact(f0[1]), "chunk 0 frame 1 bits");
        Assert.Equal(135, FrameBits.Exact(f0[31]), "chunk 0 frame 31 bits");

        var d1 = DataMsg.Build(Sess, 216, stream.AsSpan(216, 216));
        var f1 = FastPacket.Encode(0x1DEF2AE6, d1, 1);
        Assert.Hex("20 DF FE 9F A4 5A D8 00", f1[0].Data, "chunk 1 frame 0");
        Assert.Hex("21 00 D8 D9 DA DB DC DD", f1[1].Data, "chunk 1 frame 1");
        Assert.Hex("3F A9 AA AB AC AD AE AF", f1[31].Data, "chunk 1 frame 31");
        Assert.Equal(139, FrameBits.Exact(f1[0]), "chunk 1 frame 0 bits");
        Assert.Equal(135, FrameBits.Exact(f1[1]), "chunk 1 frame 1 bits");
        Assert.Equal(135, FrameBits.Exact(f1[31]), "chunk 1 frame 31 bits");
    }

    [Test]
    public static void Acks()
    {
        var first = new AckMsg { SessionId = Sess, State = SessionState.Receiving, Base = 0, End = 13824 };
        Assert.Hex("FE 9F A5 01 5A 03 00 00 00 00 00 36 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", first.Encode(), "first credit");
        Frames(0x19EFE62A, first.Encode(), 0,
            ["00 1C FE 9F A5 01 5A 03", "01 00 00 00 00 00 36 00", "02 00 00 00 00 00 00 00", "03 00 00 00 00 00 00 00", "04 00 FF FF FF FF FF FF"],
            [138, 145, 144, 145, 146], "first credit");

        ulong bm = ulong.MaxValue & ~(1UL << 5) & ~(1UL << 17);
        var holes = new AckMsg { SessionId = Sess, State = SessionState.Receiving, Base = 0, End = 13824, Bitmap = bm };
        Assert.Hex("FE 9F A5 01 5A 03 00 00 00 00 00 36 00 00 DF FF FD FF FF FF FF FF 00 00 00 00 00 00", holes.Encode(), "holes 5 and 17");

        var proc = new AckMsg { SessionId = Sess, State = SessionState.Processing, Base = 13824, End = 13824, StreamCrc32 = 0xF862F03B, HoldMs = 400 };
        Assert.Hex("FE 9F A5 01 5A 04 00 36 00 00 00 36 00 00 00 00 00 00 00 00 00 00 3B F0 62 F8 90 01", proc.Encode(), "window 1 complete");

        var w2 = new AckMsg { SessionId = Sess, State = SessionState.Receiving, Base = 13824, End = 27648, StreamCrc32 = 0xF862F03B };
        Assert.Hex("FE 9F A5 01 5A 03 00 36 00 00 00 6C 00 00 00 00 00 00 00 00 00 00 3B F0 62 F8 00 00", w2.Encode(), "window 2 credit");
    }

    [Test]
    public static void StatusResultApplyAbortIdentify()
    {
        var st = new StatusMsg
        {
            SessionId = Sess, State = SessionState.Verifying, StreamAccepted = 1_313_452, StreamSize = 1_313_452,
            ImageWritten = 2_441_792, ImageSize = 2_441_792, Seconds = 30,
        };
        Assert.Hex("FE 9F A9 01 5A 05 00 AC 0A 14 00 AC 0A 14 00 40 42 25 00 40 42 25 00 1E 00", st.Encode(), "STATUS verifying");

        var res = new ResultMsg { SessionId = Sess, Code = Reason.Ok, Phase = ResultPhase.Verified, Sha256 = ImgSha };
        Assert.Hex("""
            FE 9F A6 01 5A 00 01 00 EE 2F 7F E7 0F D1 34 41 0B 3E 0C DB 41 AB 19 30
            90 35 DA 79 69 68 51 FA C2 AC 92 5C C2 62 62 CB
            """, res.Encode(), "RESULT verified");

        var apply = new ApplyMsg { SessionId = Sess, Mode = ApplyWireMode.Now };
        Assert.Hex("FE 9F A7 01 5A 00", apply.Encode(), "APPLY");
        Frames(0x19EF2AE6, apply.Encode(), 0, ["00 06 FE 9F A7 01 5A 00"], [139], "APPLY");

        var abort = new AbortMsg { SessionId = Sess, Reason = Reason.UserCancelled };
        Assert.Hex("FE 9F A8 01 5A 17", abort.Encode(), "ABORT");
        Frames(0x19EFE62A, abort.Encode(), 0, ["00 06 FE 9F A8 01 5A 17"], [139], "ABORT");

        var ident = new IdentifyMsg { SessionId = 0, Seconds = 10, TargetUid = Uid };
        Assert.Hex("FE 9F AA 01 00 0A 02 00 00 00 00 01", ident.Encode(), "IDENTIFY");
        Frames(0x19EF2AE6, ident.Encode(), 0, ["00 0C FE 9F AA 01 00 0A", "01 02 00 00 00 00 01 FF"], [141, 145], "IDENTIFY");
    }

    [Test]
    public static void Descriptor()
    {
        var d = new FwDescriptor
        {
            Board = BoardId.B7, PanelWidth = 1024, PanelHeight = 600, FwVersion = "1.3.0", BoardName = "waveshare-7b-1024x600",
        };
        Assert.Hex("""
            4E 50 46 57 44 45 53 43 01 02 40 00 00 04 58 02 01 01 00 00 31 2E 33 2E 30 00 00 00
            00 00 00 00 00 00 00 00 77 61 76 65 73 68 61 72 65 2D 37 62 2D 31 30 32 34 78 36 30
            30 00 00 00 00 00 00 00
            """, d.Encode(), "descriptor");
        var back = FwDescriptor.Parse(d.Encode());
        Assert.Equal(d, back, "descriptor round trip");
    }

    [Test]
    public static void InflateSelfTestVector()
    {
        byte[] raw = Convert.FromHexString("F34B2C2DC94C0EC8CCCB4B4C4E55C8CC4BCB492C4955284ECD49D32D492D2E51F023200F00");
        Assert.Equal(37, raw.Length, "vector length");
        byte[] plain = DeflateCache.Inflate(raw, 1000);
        string expected = "NauticPinnace inflate self-test NauticPinnace inflate self-test ";
        Assert.Equal(expected, Encoding.ASCII.GetString(plain), "inflated text");
        // And our own encoder produces a raw stream the same decoder accepts.
        Assert.Hex(Convert.ToHexString(plain), DeflateCache.Inflate(DeflateCache.Deflate(plain), 1000), "round trip");
        using var ms = new MemoryStream(raw);
        using var ds = new DeflateStream(ms, CompressionMode.Decompress);
        Assert.Equal(64, new StreamReader(ds).ReadToEnd().Length, "64 plaintext bytes");
    }

    [Test]
    public static void FrameBitCalculator()
    {
        Assert.Equal(145, FrameBits.Exact(0x1DEF2AE6, new byte[8]), "all-zero data");
        Assert.Equal(145, FrameBits.Exact(0x1DEF2AE6, Enumerable.Repeat((byte)0xFF, 8).ToArray()), "all-0xFF data");
        Assert.True(FrameBits.Exact(0x1DEF2AE6, new byte[8]) <= FrameBits.Conservative, "never above the worst case");
    }
}
