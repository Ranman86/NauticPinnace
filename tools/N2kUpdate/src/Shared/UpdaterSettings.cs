using System.Text.Json;

namespace NauticPinnace.N2kUpdate.Hosting;

/// <summary>
/// Settings of the standalone updater in %APPDATA%\NauticPinnaceUpdater\settings.json
/// (12.3). The 21-bit unique number of the NAME is drawn once and kept, so that a
/// chartplotter lists the updater as one device across runs.
/// </summary>
public sealed class UpdaterSettings
{
    public int UniqueNumber { get; set; }
    public int PreferredAddress { get; set; } = 230;
    public int BusSharePercent { get; set; } = 30;
    public bool AdaptiveBackoff { get; set; } = true;
    public bool Compression { get; set; } = true;
    public string ApplyMode { get; set; } = "Now";
    public string Language { get; set; } = "de";
    public string? LastRelease { get; set; }
    public string? LastPort { get; set; }

    public static string Folder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "NauticPinnaceUpdater");

    public static string LocalFolder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NauticPinnaceUpdater");

    /// <summary>The deflate cache of 12.4.</summary>
    public static string CacheFolder => Path.Combine(LocalFolder, "cache");

    public static string LogFolder => Path.Combine(LocalFolder, "logs");

    public static string FilePath => Path.Combine(Folder, "settings.json");

    public static UpdaterSettings Load()
    {
        UpdaterSettings s;
        try
        {
            s = File.Exists(FilePath)
                ? JsonSerializer.Deserialize<UpdaterSettings>(File.ReadAllText(FilePath)) ?? new UpdaterSettings()
                : new UpdaterSettings();
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            s = new UpdaterSettings();
        }
        if (s.UniqueNumber <= 0 || s.UniqueNumber > 0x1FFFFF)
        {
            s.UniqueNumber = Random.Shared.Next(1, 0x200000);
            s.Save();
        }
        s.BusSharePercent = Math.Clamp(s.BusSharePercent, 10, 60);
        return s;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Folder);
            string tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
            File.Move(tmp, FilePath, overwrite: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // Settings are a convenience: never fail because of them.
        }
    }
}
