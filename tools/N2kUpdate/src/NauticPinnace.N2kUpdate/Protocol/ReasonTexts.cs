namespace NauticPinnace.N2kUpdate.Protocol;

/// <summary>
/// The DE and EN texts of table 4.8, for senders that show a reason to a person.
/// An unknown code is shown by its number (4.8, last paragraph).
/// </summary>
public static class ReasonTexts
{
    public static string Describe(Reason r, bool german)
    {
        string? t = german ? De(r) : En(r);
        return t ?? (german ? $"Fehler 0x{(byte)r:X2}" : $"Error 0x{(byte)r:X2}");
    }

    /// <summary>The symbolic name plus the code, for log files.</summary>
    public static string Code(Reason r) =>
        Enum.IsDefined(r) ? $"{r} (0x{(byte)r:X2})" : $"0x{(byte)r:X2}";

    private static string? De(Reason r) => r switch
    {
        Reason.Ok                  => "OK",
        Reason.DeclinedByUser      => "Am Display abgelehnt",
        Reason.ConfirmTimeout      => "Am Display nicht bestätigt",
        Reason.Disabled            => "Updates über NMEA 2000 sind ausgeschaltet",
        Reason.Busy                => "Ein anderes Update läuft",
        Reason.WrongBoard          => "Firmware für ein anderes Gerät",
        Reason.TooLarge            => "Datei zu groß",
        Reason.UnsupportedTarget   => "Nicht unterstützt",
        Reason.UnsupportedEncoding => "Nicht unterstützt",
        Reason.UnsupportedProtocol => "Nicht unterstützt",
        Reason.BadRequest          => "Ungültige Anfrage",
        Reason.PendingVerify       => "Das letzte Update wird noch geprüft",
        Reason.AlarmActive         => "Alarm aktiv",
        Reason.WrongDevice         => "Falsches Gerät",
        Reason.VersionMismatch     => "Weboberfläche passt nicht zur Firmware",
        Reason.NoSpace             => "Zu wenig Platz im Dateisystem",
        Reason.OutOfMemory         => "Zu wenig Arbeitsspeicher",
        Reason.DescriptorInvalid   => "Firmware ohne gültige Kennung",
        Reason.ShaMismatch         => "Prüfsumme falsch – Daten beschädigt",
        Reason.ImageInvalid        => "Firmware-Abbild ungültig",
        Reason.SizeMismatch        => "Größe stimmt nicht",
        Reason.FlashError          => "Fehler beim Schreiben",
        Reason.Timeout             => "Verbindung zum Absender verloren",
        Reason.UserCancelled       => "Am Display abgebrochen",
        Reason.SenderCancelled     => "Vom Absender abgebrochen",
        Reason.SessionUnknown      => "Sitzung auf dem Display unbekannt",
        Reason.StreamCrcMismatch   => "Übertragungsfehler",
        Reason.LinkUnreliable      => "Bus zu unzuverlässig",
        Reason.ApplyFailed         => "Aktivieren fehlgeschlagen",
        Reason.RestartPending      => "Neustart ausstehend",
        Reason.NotReady            => "Gerät startet noch",
        Reason.InternalError       => "Interner Fehler",
        _                          => null,
    };

    private static string? En(Reason r) => r switch
    {
        Reason.Ok                  => "OK",
        Reason.DeclinedByUser      => "Declined on the display",
        Reason.ConfirmTimeout      => "Not confirmed on the display",
        Reason.Disabled            => "Updates over NMEA 2000 are switched off",
        Reason.Busy                => "Another update is in progress",
        Reason.WrongBoard          => "Firmware for a different device",
        Reason.TooLarge            => "File too large",
        Reason.UnsupportedTarget   => "Not supported",
        Reason.UnsupportedEncoding => "Not supported",
        Reason.UnsupportedProtocol => "Not supported",
        Reason.BadRequest          => "Invalid request",
        Reason.PendingVerify       => "The previous update is still being checked",
        Reason.AlarmActive         => "Alarm active",
        Reason.WrongDevice         => "Wrong device",
        Reason.VersionMismatch     => "Web interface does not match the firmware",
        Reason.NoSpace             => "Not enough space in the file system",
        Reason.OutOfMemory         => "Out of memory",
        Reason.DescriptorInvalid   => "Firmware without a valid identification",
        Reason.ShaMismatch         => "Checksum mismatch – data corrupted",
        Reason.ImageInvalid        => "Firmware image invalid",
        Reason.SizeMismatch        => "Size mismatch",
        Reason.FlashError          => "Write error",
        Reason.Timeout             => "Connection to the sender lost",
        Reason.UserCancelled       => "Cancelled on the display",
        Reason.SenderCancelled     => "Cancelled by the sender",
        Reason.SessionUnknown      => "Session unknown to the display",
        Reason.StreamCrcMismatch   => "Transmission error",
        Reason.LinkUnreliable      => "Bus too unreliable",
        Reason.ApplyFailed         => "Activation failed",
        Reason.RestartPending      => "Restart pending",
        Reason.NotReady            => "Device is still starting",
        Reason.InternalError       => "Internal error",
        _                          => null,
    };
}
