using System.IO.Compression;
using NauticPinnace.N2kUpdate.Protocol;

namespace NauticPinnace.N2kUpdate.Images;

/// <summary>The files of one board within a release.</summary>
public sealed class BoardPackage
{
    public BoardId Board { get; init; }
    public FirmwareImage? Firmware { get; init; }
    public WebUiImage? WebUi { get; init; }
    /// <summary>Where the web UI came from, for the log and the UI.</summary>
    public string? WebUiOrigin { get; init; }

    /// <summary>The firmware version the web UI belongs to: the descriptor version of the same folder (12.4).</summary>
    public string? WebUiVersion => Firmware?.Descriptor?.FwVersion;
}

/// <summary>
/// A release as the standalone updater accepts it (12.4): a release folder or ZIP
/// with the subfolders 4/, 7b/ and 5b/ (the layout of the release ZIP), a single
/// board folder, a PlatformIO build folder, or single files.
///
/// Where the web UI comes from, per board folder, first match wins:
///   1. index.html.gz in the board folder (release layout from 11.6 on);
///   2. littlefs.bin in the board folder: index.html.gz is read out of the LittleFS
///      image (release ZIPs up to v1.2.0 carry it only there);
///   3. index.html.gz in the selected root folder;
///   4. data/index.html.gz below the selected root (a repository checkout).
/// </summary>
public sealed class ReleasePackage
{
    private readonly Dictionary<BoardId, BoardPackage> _boards;

    private ReleasePackage(string source, Dictionary<BoardId, BoardPackage> boards, List<string> notes)
    {
        Source = source;
        _boards = boards;
        Notes = notes;
    }

    public string Source { get; }
    public IReadOnlyDictionary<BoardId, BoardPackage> Boards => _boards;
    public IReadOnlyList<string> Notes { get; }

    public BoardPackage? For(BoardId board) => _boards.GetValueOrDefault(board);

    /// <summary>Loads a folder or a .zip file.</summary>
    public static ReleasePackage Load(string path)
    {
        if (File.Exists(path) && path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) return LoadZip(path);
        if (Directory.Exists(path)) return LoadFolder(path);
        throw new FileNotFoundException("neither a folder nor a .zip file", path);
    }

    public static ReleasePackage LoadFolder(string root)
    {
        var files = new FolderSource(root);
        return Build(root, files);
    }

    public static ReleasePackage LoadZip(string zipPath)
    {
        using var zip = ZipFile.OpenRead(zipPath);
        var files = new ZipSource(zip);
        return Build(zipPath, files);
    }

    /// <summary>
    /// Single files. A web UI may also be given as a littlefs.bin. The web UI version
    /// comes from the firmware if one is given, otherwise the user enters it (12.4).
    /// </summary>
    public static (FirmwareImage? Firmware, WebUiImage? WebUi) LoadFiles(string? firmwarePath, string? webUiPath)
    {
        FirmwareImage? fw = string.IsNullOrEmpty(firmwarePath) ? null : FirmwareImage.Load(firmwarePath);
        WebUiImage? web = null;
        if (!string.IsNullOrEmpty(webUiPath))
        {
            if (webUiPath.EndsWith(".bin", StringComparison.OrdinalIgnoreCase))
            {
                byte[]? gz = ExtractWebUi(File.ReadAllBytes(webUiPath));
                if (gz is null) throw new InvalidDataException($"{webUiPath}: no index.html.gz in this LittleFS image");
                web = WebUiImage.FromBytes(gz, webUiPath + ":/index.html.gz");
            }
            else web = WebUiImage.Load(webUiPath);
        }
        return (fw, web);
    }

    /// <summary>Reads /index.html.gz out of a littlefs.bin, or null.</summary>
    public static byte[]? ExtractWebUi(byte[] littleFsImage) =>
        LittleFsReader.TryOpen(littleFsImage)?.ReadRootFile("index.html.gz");

    // ── Building ──────────────────────────────────────────────────────────────
    private interface ISource
    {
        /// <summary>Relative paths with '/' separators.</summary>
        IEnumerable<string> Files { get; }
        byte[] Read(string relative);
        string Describe(string relative);
    }

    private static ReleasePackage Build(string source, ISource src)
    {
        var notes = new List<string>();
        var all = src.Files.ToList();
        var set = new HashSet<string>(all, StringComparer.OrdinalIgnoreCase);

        // Shared web UI candidates of the selected root.
        string? rootWeb = all.FirstOrDefault(f => f.Equals("index.html.gz", StringComparison.OrdinalIgnoreCase))
                       ?? all.Where(f => f.EndsWith("data/index.html.gz", StringComparison.OrdinalIgnoreCase) && Depth(f) <= 2)
                             .OrderBy(Depth).FirstOrDefault();

        var boards = new Dictionary<BoardId, BoardPackage>();
        foreach (string fwPath in all.Where(f => Name(f).Equals("firmware.bin", StringComparison.OrdinalIgnoreCase) && Depth(f) <= 3)
                                     .OrderBy(Depth).ThenBy(f => f, StringComparer.OrdinalIgnoreCase))
        {
            FirmwareImage fw;
            try { fw = FirmwareImage.FromBytes(src.Read(fwPath), src.Describe(fwPath)); }
            catch (Exception ex) when (ex is IOException or InvalidDataException) { notes.Add($"{fwPath}: {ex.Message}"); continue; }

            BoardId? board = fw.Descriptor?.Board ?? fw.LegacyBoard ?? BoardFromFolder(Dir(fwPath));
            if (board is null || board == BoardId.Any) { notes.Add($"{fwPath}: board unknown"); continue; }
            if (boards.ContainsKey(board.Value)) { notes.Add($"{fwPath}: a second firmware for {Protocol.Boards.DisplayName(board.Value)}, ignored"); continue; }

            string dir = Dir(fwPath);
            WebUiImage? web = null;
            string? origin = null;
            string localGz = Join(dir, "index.html.gz"), localLfs = Join(dir, "littlefs.bin");
            if (set.Contains(localGz))
            {
                web = WebUiImage.FromBytes(src.Read(localGz), src.Describe(localGz));
                origin = localGz;
            }
            else if (set.Contains(localLfs))
            {
                try
                {
                    byte[]? gz = ExtractWebUi(src.Read(localLfs));
                    if (gz is not null)
                    {
                        web = WebUiImage.FromBytes(gz, src.Describe(localLfs) + ":/index.html.gz");
                        origin = localLfs + ":/index.html.gz";
                    }
                    else notes.Add($"{localLfs}: no index.html.gz inside");
                }
                catch (Exception ex) when (ex is IOException or InvalidDataException) { notes.Add($"{localLfs}: {ex.Message}"); }
            }
            if (web is null && rootWeb is not null)
            {
                web = WebUiImage.FromBytes(src.Read(rootWeb), src.Describe(rootWeb));
                origin = rootWeb;
            }
            boards[board.Value] = new BoardPackage { Board = board.Value, Firmware = fw, WebUi = web, WebUiOrigin = origin };
        }

        if (boards.Count == 0 && rootWeb is not null)
        {
            // Only a web UI (for example a repository checkout without builds).
            var web = WebUiImage.FromBytes(src.Read(rootWeb), src.Describe(rootWeb));
            boards[BoardId.Any] = new BoardPackage { Board = BoardId.Any, WebUi = web, WebUiOrigin = rootWeb };
        }
        if (boards.Count == 0) notes.Add("no firmware.bin and no index.html.gz found");
        return new ReleasePackage(source, boards, notes);
    }

    private static BoardId? BoardFromFolder(string dir)
    {
        string last = Name(dir).ToLowerInvariant();
        return last switch
        {
            "4" or "waveshare_esp32s3_4" => BoardId.Inch4,
            "7b" or "waveshare_esp32s3_7b" => BoardId.B7,
            "5b" or "waveshare_esp32s3_5b" => BoardId.B5,
            _ => null,
        };
    }

    private static int Depth(string rel) => rel.Count(c => c == '/');
    private static string Name(string rel) { int i = rel.LastIndexOf('/'); return i < 0 ? rel : rel[(i + 1)..]; }
    private static string Dir(string rel) { int i = rel.LastIndexOf('/'); return i < 0 ? "" : rel[..i]; }
    private static string Join(string dir, string name) => dir.Length == 0 ? name : dir + "/" + name;

    private sealed class FolderSource(string root) : ISource
    {
        // Folders that never hold a release file but can be huge in a repository checkout.
        private static readonly HashSet<string> Skip = new(StringComparer.OrdinalIgnoreCase)
            { ".git", "managed_components", "node_modules", "libdeps", "obj", "src", "lib", "sim", "LICENSES", "esptool-4.8.1" };

        public IEnumerable<string> Files => Walk(root, 0)
            .Where(f => f.EndsWith(".bin", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".gz", StringComparison.OrdinalIgnoreCase))
            .Select(f => Path.GetRelativePath(root, f).Replace('\\', '/'));

        private static IEnumerable<string> Walk(string dir, int depth)
        {
            IEnumerable<string> files, dirs;
            try
            {
                files = Directory.EnumerateFiles(dir).ToList();
                dirs = depth < 3 ? Directory.EnumerateDirectories(dir).ToList() : [];
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { yield break; }
            foreach (var f in files) yield return f;
            foreach (var d in dirs)
            {
                if (Skip.Contains(Path.GetFileName(d))) continue;
                foreach (var f in Walk(d, depth + 1)) yield return f;
            }
        }

        public byte[] Read(string relative) => File.ReadAllBytes(Path.Combine(root, relative));
        public string Describe(string relative) => Path.Combine(root, relative);
    }

    private sealed class ZipSource(ZipArchive zip) : ISource
    {
        public IEnumerable<string> Files => zip.Entries.Where(e => e.Length > 0).Select(e => e.FullName.Replace('\\', '/'));

        public byte[] Read(string relative)
        {
            var e = zip.Entries.First(x => x.FullName.Replace('\\', '/') == relative);
            using var s = e.Open();
            using var ms = new MemoryStream((int)e.Length);
            s.CopyTo(ms);
            return ms.ToArray();
        }

        public string Describe(string relative) => "zip:" + relative;
    }
}
