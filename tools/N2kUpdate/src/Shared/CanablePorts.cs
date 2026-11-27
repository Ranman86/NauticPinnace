using System.IO.Ports;

namespace NauticPinnace.N2kUpdate.Hosting;

/// <summary>
/// Finds the CANable (MKS CANable v2.0 with slcan firmware, USB VID 16D0 / PID 117E).
/// Needed because its COM port wanders as soon as other USB serial devices are
/// plugged in - the displays themselves among them. The Windows part is ported from
/// the author's own earlier, unpublished slcan sender (same author, no third-party
/// code; registry instead of WMI, which would need a package). Linked into the
/// hosts, not into the library.
/// </summary>
public static class CanablePorts
{
    private const string Vid = "16D0", Pid = "117E";

    /// <summary>The CANable's port name (COMx or /dev/ttyACMx), or null.</summary>
    public static string? Find() => FindAll().FirstOrDefault(p => SerialPort.GetPortNames().Contains(p) || p.StartsWith("/dev/"));

    /// <summary>Every port that belongs to a CANable known to the system (present or not on Windows).</summary>
    public static IEnumerable<string> FindAll()
    {
        if (OperatingSystem.IsWindows())
        {
            foreach (var p in FromRegistry()) yield return p;
            yield break;
        }
        if (OperatingSystem.IsLinux())
            foreach (var p in FromSysfs()) yield return p;
    }

    /// <summary>All serial ports, sorted by number (COM3 before COM10).</summary>
    public static string[] AllPorts() =>
        SerialPort.GetPortNames().Distinct().OrderBy(Number).ThenBy(p => p, StringComparer.Ordinal).ToArray();

    private static int Number(string p)
    {
        var digits = new string(p.Where(char.IsDigit).ToArray());
        return int.TryParse(digits, out int n) ? n : int.MaxValue;
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    private static List<string> FromRegistry()
    {
        var list = new List<string>();
        try
        {
            using var usb = Microsoft.Win32.Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Enum\USB\VID_{Vid}&PID_{Pid}");
            if (usb is null) return list;
            foreach (string inst in usb.GetSubKeyNames())
            {
                using var parms = usb.OpenSubKey(inst + @"\Device Parameters");
                if (parms?.GetValue("PortName") is string port && port.StartsWith("COM", StringComparison.Ordinal))
                    list.Add(port);
            }
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException)
        {
            // No access: simply suggest nothing.
        }
        return list;
    }

    private static List<string> FromSysfs()
    {
        var list = new List<string>();
        try
        {
            foreach (string tty in Directory.EnumerateDirectories("/sys/class/tty", "ttyACM*"))
            {
                // /sys/class/tty/ttyACM0/device -> the USB interface; idVendor sits one level up.
                string dev = Path.Combine(tty, "device", "..");
                string vid = ReadTrim(Path.Combine(dev, "idVendor")), pid = ReadTrim(Path.Combine(dev, "idProduct"));
                if (vid.Equals(Vid, StringComparison.OrdinalIgnoreCase) && pid.Equals(Pid, StringComparison.OrdinalIgnoreCase))
                    list.Add("/dev/" + Path.GetFileName(tty));
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return list;
    }

    private static string ReadTrim(string path)
    {
        try { return File.Exists(path) ? File.ReadAllText(path).Trim() : ""; }
        catch (IOException) { return ""; }
    }
}
