using System.IO.Compression;
using System.Text;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Testing;

namespace NauticPinnace.N2kUpdate.Tests;

/// <summary>Descriptor parsing, image id, legacy detection, deflate cache, release folders (8, 12.4).</summary>
public static class ImageTests
{
    /// <summary>The repository root (the folder with platformio.ini), or null when the tests run elsewhere.</summary>
    public static string? RepoRoot()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "platformio.ini"))) return d.FullName;
        return null;
    }

    /// <summary>The v1.2.0 release folder (release/ is not in git, so it may be missing).</summary>
    public static string? Release120()
    {
        string? root = RepoRoot();
        if (root is null) return null;
        string p = Path.Combine(root, "release", "NauticPinnace-v1.2.0");
        return Directory.Exists(Path.Combine(p, "7b")) ? p : null;
    }

    private static void Skip(string why) => Console.WriteLine("      (skipped: " + why + ")");

    [Test]
    public static void SyntheticImageChecks()
    {
        var ok = FirmwareImage.FromBytes(TestImageFactory.Synthetic(BoardId.B7, "1.3.1", id: new ImageId(0x3F91FABF52B1CC4B)));
        Assert.True(ok.IsValid, "valid: " + string.Join("; ", ok.Problems));
        Assert.Equal(BoardId.B7, ok.Board, "board");
        Assert.Equal("1.3.1", ok.Version, "version");
        Assert.Equal("3F91FABF52B1CC4B", ok.ImageId.ToString(), "image id at 0xB0");
        var info = VectorTests.SampleInfo();
        Assert.Equal(0, ok.CheckAgainst(info).Count, "fits a 7B");
        Assert.True(ok.CheckAgainst(info with { Board = BoardId.Inch4 }).Count > 0, "refused for a 4-inch");
        Assert.True(ok.CheckAgainst(info with { MaxFirmwareSize = 1000 }).Count > 0, "refused when too large");

        byte[] noGuard = TestImageFactory.Synthetic(BoardId.B7, "1.3.1");
        noGuard[FwDescriptor.Offset + 0x11] = 0;
        Assert.True(!FirmwareImage.FromBytes(noGuard).IsValid, "OTAGUARD flag missing is refused");

        byte[] wrongPanel = TestImageFactory.Synthetic(BoardId.B7, "1.3.1");
        wrongPanel[FwDescriptor.Offset + 0x0C] = 0xE0; wrongPanel[FwDescriptor.Offset + 0x0D] = 0x01;   // 480 wide
        Assert.True(!FirmwareImage.FromBytes(wrongPanel).IsValid, "panel_w that does not match the board is refused");

        byte[] longVersion = TestImageFactory.Synthetic(BoardId.B5, "1234567890123456"[..15]);
        Assert.True(FirmwareImage.FromBytes(longVersion).IsValid, "15-character version is fine");

        byte[] notEsp = TestImageFactory.Synthetic(BoardId.B5, "1.3.1");
        notEsp[0] = 0;
        Assert.True(!FirmwareImage.FromBytes(notEsp).IsValid, "byte 0 must be 0xE9");

        var web = WebUiImage.FromBytes(TestImageFactory.SyntheticWebUi());
        Assert.True(web.IsValid, "gzip web UI is valid");
        Assert.True(!WebUiImage.FromBytes([0x3C, 0x68, 0x74]).IsValid, "plain HTML is refused");
        Assert.True(web.CheckAgainst(info with { MaxWebUiSize = 100 }).Count > 0, "web UI larger than the region is refused");
    }

    [Test]
    public static void LegacyImagesOfRelease120()
    {
        string? rel = Release120();
        if (rel is null) { Skip("release/NauticPinnace-v1.2.0 not present"); return; }
        foreach (var (folder, board) in new[] { ("4", BoardId.Inch4), ("7b", BoardId.B7), ("5b", BoardId.B5) })
        {
            var fw = FirmwareImage.Load(Path.Combine(rel, folder, "firmware.bin"));
            Assert.True(fw.IsLegacy, $"{folder}: legacy");
            Assert.Equal(board, fw.LegacyBoard, $"{folder}: board from FW_BOARD_NAME");
            var problems = fw.CheckAgainst(VectorTests.SampleInfo());
            Assert.True(problems.Count == 1 && problems[0].Contains("install over WiFi or USB"), $"{folder}: refused with the legacy hint");

            // With a descriptor (test image only) it passes every structural check.
            var patched = FirmwareImage.FromBytes(TestImageFactory.AddDescriptor(fw.Bytes, board, "1.3.1"));
            Assert.True(patched.IsValid, $"{folder}: patched image valid: " + string.Join("; ", patched.Problems));
            Assert.Equal(fw.ImageId, patched.ImageId, $"{folder}: image id unchanged");
        }
    }

    [Test]
    public static void WebUiFromLittleFs()
    {
        string? rel = Release120();
        if (rel is null) { Skip("release/NauticPinnace-v1.2.0 not present"); return; }
        byte[] lfs = File.ReadAllBytes(Path.Combine(rel, "7b", "littlefs.bin"));
        var reader = LittleFsReader.TryOpen(lfs) ?? throw new TestFailure("littlefs.bin not recognised");
        var names = reader.RootFileNames().ToList();
        Assert.True(names.Contains("index.html.gz") && names.Contains("config.json"), "root files: " + string.Join(", ", names));
        byte[] gz = reader.ReadRootFile("index.html.gz") ?? throw new TestFailure("no index.html.gz");
        var web = WebUiImage.FromBytes(gz);
        Assert.True(web.IsValid, "gzip header");
        using var ds = new GZipStream(new MemoryStream(gz), CompressionMode.Decompress);
        string html = new StreamReader(ds, Encoding.UTF8).ReadToEnd();
        Assert.True(html.Contains("<html", StringComparison.OrdinalIgnoreCase) && html.Contains("NauticPinnace"), "decompresses to the web UI");

        // The plain index.html in the same image must decompress to the same document.
        byte[]? plain = reader.ReadRootFile("index.html");
        if (plain is not null) Assert.Equal(Encoding.UTF8.GetString(plain), html, "index.html.gz matches index.html");
    }

    [Test]
    public static void ReleaseFolderOf120()
    {
        string? rel = Release120();
        if (rel is null) { Skip("release/NauticPinnace-v1.2.0 not present"); return; }
        var pkg = ReleasePackage.Load(rel);
        Assert.Equal(3, pkg.Boards.Count, "three boards: " + string.Join("; ", pkg.Notes));
        foreach (var b in Boards.Known)
        {
            var bp = pkg.For(b) ?? throw new TestFailure($"no package for {b}");
            Assert.True(bp.Firmware!.IsLegacy, $"{b}: legacy firmware");
            Assert.True(bp.WebUi is { IsValid: true }, $"{b}: web UI taken from littlefs.bin");
            Assert.True(bp.WebUiOrigin!.EndsWith("littlefs.bin:/index.html.gz"), $"{b}: origin {bp.WebUiOrigin}");
        }
        string zip = rel + ".zip";
        if (File.Exists(zip))
        {
            var z = ReleasePackage.Load(zip);
            Assert.Equal(3, z.Boards.Count, "three boards in the ZIP");
            Assert.Equal(pkg.For(BoardId.B7)!.WebUi!.WebUiId, z.For(BoardId.B7)!.WebUi!.WebUiId, "same web UI from folder and ZIP");
        }
    }

    [Test]
    public static void ReleaseFolderLayouts()
    {
        string dir = Path.Combine(Path.GetTempPath(), "n2kupd-rel-" + Guid.NewGuid().ToString("N"));
        try
        {
            byte[] fw7 = TestImageFactory.Synthetic(BoardId.B7, "1.4.0"), fw4 = TestImageFactory.Synthetic(BoardId.Inch4, "1.4.0");
            byte[] web = TestImageFactory.SyntheticWebUi();
            Directory.CreateDirectory(Path.Combine(dir, "7b"));
            Directory.CreateDirectory(Path.Combine(dir, "4"));
            File.WriteAllBytes(Path.Combine(dir, "7b", "firmware.bin"), fw7);
            File.WriteAllBytes(Path.Combine(dir, "7b", "index.html.gz"), web);
            File.WriteAllBytes(Path.Combine(dir, "4", "firmware.bin"), fw4);
            Directory.CreateDirectory(Path.Combine(dir, "data"));
            File.WriteAllBytes(Path.Combine(dir, "data", "index.html.gz"), TestImageFactory.SyntheticWebUi(seed: 9));
            var pkg = ReleasePackage.Load(dir);
            Assert.Equal(2, pkg.Boards.Count, "two boards");
            Assert.Equal("7b/index.html.gz", pkg.For(BoardId.B7)!.WebUiOrigin, "board folder first");
            Assert.Equal("data/index.html.gz", pkg.For(BoardId.Inch4)!.WebUiOrigin, "repository data/ as fallback");
            Assert.Equal("1.4.0", pkg.For(BoardId.B7)!.WebUiVersion, "web UI version from the firmware in the same folder");

            string zip = dir + ".zip";
            ZipFile.CreateFromDirectory(dir, zip, CompressionLevel.Fastest, includeBaseDirectory: true);
            var z = ReleasePackage.Load(zip);
            Assert.Equal(2, z.Boards.Count, "ZIP with a top folder");
            File.Delete(zip);
        }
        finally { Directory.Delete(dir, recursive: true); }
    }

    [Test]
    public static void VersionBytesMustBePrintableAscii()
    {
        // 8.1: fw_version is 1-15 bytes 0x20-0x7E. Decoded as ASCII a 0xC3 would read
        // '?' and pass; the display compares the bytes and would refuse it only after
        // the user confirmed (DESCRIPTOR_INVALID at the first unit, 8.2).
        var img = TestImageFactory.Synthetic(BoardId.B7, "1.3.1", 20_000);
        Assert.True(FirmwareImage.FromBytes(img).IsValid, "clean version");
        img[FwDescriptor.Offset + 0x14 + 3] = 0xC3;
        Assert.True(!FirmwareImage.FromBytes(img).IsValid, "non-ASCII byte refused");
        img = TestImageFactory.Synthetic(BoardId.B7, "1.3.1", 20_000);
        img[FwDescriptor.Offset + 0x14 + 10] = (byte)'x';   // a byte after the NUL
        Assert.True(!FirmwareImage.FromBytes(img).IsValid, "not NUL-padded refused");
    }

    [Test]
    public static void DeflateCacheIsByteIdenticalAfterRestart()
    {
        string dir = Path.Combine(Path.GetTempPath(), "n2kupd-cache-" + Guid.NewGuid().ToString("N"));
        try
        {
            var fw = FirmwareImage.FromBytes(TestImageFactory.Synthetic(BoardId.B7, "1.3.1", 200_000));
            var c1 = new DeflateCache(dir);
            var t1 = TransferImage.ForFirmware(fw, deflate: true, c1);
            Assert.Equal(StreamEncoding.Deflate, t1.Encoding, "deflate used");
            Assert.True(t1.StreamSize < fw.Size, "smaller than the image");
            Assert.True(File.Exists(c1.PathFor(fw.Sha256)!), "cached on disk");
            Assert.True(c1.PathFor(fw.Sha256)!.EndsWith("-" + DeflateCache.EncoderId + ".deflate"), "keyed by SHA-256 and encoder id");
            Assert.Hex(Convert.ToHexString(fw.Bytes), DeflateCache.Inflate(t1.Stream, fw.Size), "inflates to the image");

            // "Restart": a new cache instance reads the stream back from disk.
            var c2 = new DeflateCache(dir);
            var t2 = TransferImage.ForFirmware(fw, deflate: true, c2);
            Assert.True(c2.LastWasFromDisk, "taken from the disk cache");
            Assert.Equal(t1.StreamCrc32, t2.StreamCrc32, "byte-identical stream (stream CRC)");

            // A damaged cache entry is detected and rebuilt.
            var path = c2.PathFor(fw.Sha256)!;
            var bad = File.ReadAllBytes(path);
            bad[bad.Length / 2] ^= 0xFF;
            File.WriteAllBytes(path, bad);
            var c3 = new DeflateCache(dir);
            var t3 = TransferImage.ForFirmware(fw, deflate: true, c3);
            Assert.True(!c3.LastWasFromDisk, "damaged entry not used");
            Assert.Hex(Convert.ToHexString(fw.Bytes), DeflateCache.Inflate(t3.Stream, fw.Size), "rebuilt stream inflates correctly");

            // Bytes after the final deflate block: inflating alone would not notice, the
            // display would reject the stream only after the whole transfer (SIZE_MISMATCH).
            File.WriteAllBytes(path, File.ReadAllBytes(path).Concat(new byte[] { 1, 2, 3, 4 }).ToArray());
            var c4 = new DeflateCache(dir);
            var t4 = TransferImage.ForFirmware(fw, deflate: true, c4);
            Assert.True(!c4.LastWasFromDisk, "entry with trailing bytes not used");
            Assert.Equal(t1.StreamCrc32, t4.StreamCrc32, "rebuilt byte-identical");

            var raw = TransferImage.ForFirmware(fw, deflate: false);
            Assert.Equal(raw.ImageSize, raw.StreamSize, "raw: stream = image");
            Assert.True(t1.StreamSize <= UpdProto.StreamBound(fw.Size), "within the bound of 3.3");
        }
        finally { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
    }
}
