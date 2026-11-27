using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;

namespace NauticPinnace.Updater;

/// <summary>
/// Every text of the updater, German and English side by side - the one place for
/// UI strings. German is the default; the language follows the setting.
/// </summary>
internal static class T
{
    public static bool German { get; set; } = true;

    private static string S(string de, string en) => German ? de : en;

    // ── Window and groups ─────────────────────────────────────────────────────
    public static string Title => S("NauticPinnace – Update über NMEA 2000", "NauticPinnace – update over NMEA 2000");
    public static string GroupAdapter => S("CAN-Adapter (CANable)", "CAN adapter (CANable)");
    public static string GroupRelease => S("Release", "Release");
    public static string GroupDisplays => S("Displays am Bus", "Displays on the bus");
    public static string GroupOptions => S("Optionen", "Options");
    public static string GroupProgress => S("Fortschritt", "Progress");
    public static string GroupLog => S("Protokoll", "Log");

    // ── Adapter ───────────────────────────────────────────────────────────────
    public static string Port => S("Anschluss:", "Port:");
    public static string RefreshPorts => S("Aktualisieren", "Refresh");
    public static string Connect => S("Verbinden", "Connect");
    public static string Disconnect => S("Trennen", "Disconnect");
    public static string CanableSuffix => S(" (CANable)", " (CANable)");
    public static string NotConnected => S("Nicht verbunden", "Not connected");
    public static string Connecting => S("Verbinde …", "Connecting …");
    public static string ConnectedAs(int addr) => S($"Verbunden, eigene Adresse {addr}", $"Connected, own address {addr}");
    public static string BusLoad(double own, double total, double share) =>
        S($"Buslast {total:P0} (davon Updater {own:P0}, erlaubt {share:P0})", $"Bus load {total:P0} (updater {own:P0}, allowed {share:P0})");
    public static string NoCanableFound => S("Kein CANable gefunden. Bitte anstecken oder den Anschluss wählen.", "No CANable found. Plug it in or choose the port.");
    public static string NotACanable(string port) =>
        S($"{port} ist nicht als CANable erkannt. Wenn das ein Display am USB ist, startet es beim Öffnen womöglich neu.\n\nTrotzdem öffnen?",
          $"{port} is not recognised as a CANable. If it is a display on USB, opening it may restart it.\n\nOpen anyway?");
    public static string ConnectFailed(string why) => S($"Verbindung fehlgeschlagen: {why}", $"Connection failed: {why}");

    // ── Release ───────────────────────────────────────────────────────────────
    public static string ChooseFolder => S("Ordner …", "Folder …");
    public static string ChooseZip => S("ZIP …", "ZIP …");
    public static string ChooseFiles => S("Einzelne Dateien …", "Single files …");
    public static string NoRelease => S("Kein Release gewählt", "No release chosen");
    public static string FolderPrompt => S("Release-Ordner wählen (mit den Unterordnern 4, 7b, 5b) oder einen Build-Ordner", "Choose the release folder (with the subfolders 4, 7b, 5b) or a build folder");
    public static string ZipFilter => S("Release-ZIP (*.zip)|*.zip", "Release ZIP (*.zip)|*.zip");
    public static string FilesFilter => S("Firmware oder Weboberfläche (*.bin;*.gz)|*.bin;*.gz|Alle Dateien (*.*)|*.*", "Firmware or web interface (*.bin;*.gz)|*.bin;*.gz|All files (*.*)|*.*");
    public static string WebUiOnly => S("Weboberfläche", "Web interface");
    public static string LegacyFirmware => S("ohne N2K-Kennung", "without N2K descriptor");
    public static string WebUiFrom(string origin) => S($"Weboberfläche aus {origin}", $"web interface from {origin}");
    public static string LoadFailed(string why) => S($"Laden fehlgeschlagen: {why}", $"Loading failed: {why}");

    // ── Display list ──────────────────────────────────────────────────────────
    public static string ColAddress => S("Adr.", "Addr.");
    public static string ColBoard => S("Gerät", "Device");
    public static string ColLabel => S("Name", "Name");
    public static string ColVersion => S("Firmware", "Firmware");
    public static string ColState => S("Zustand", "State");
    public static string ColAccept => S("Updates", "Updates");
    public static string ColWebUi => S("Weboberfläche", "Web interface");
    public static string ColUid => S("Kennung", "ID");
    public static string ColStatus => S("Status", "Status");
    public static string Discover => S("Displays suchen", "Find displays");
    public static string Identify => S("Identifizieren", "Identify");
    public static string Install => S("Installieren", "Install");
    public static string Cancel => S("Abbrechen", "Cancel");
    public static string Searching => S("Suche …", "Searching …");
    public static string NoneFound => S("Kein Display gefunden.", "No display found.");
    public static string On => S("an", "on");
    public static string Off => S("aus", "off");
    public static string OtaValid => S("bestätigt", "validated");
    public static string OtaPending => S("wird geprüft", "being checked");
    public static string LegacyHint(string version) =>
        S($"NauticPinnace, Firmware {version}: noch kein Update über NMEA 2000. Einmal über WLAN oder USB aktualisieren.",
          $"NauticPinnace, firmware {version}: no update over NMEA 2000 yet; update it once over WiFi or USB.");
    public static string IdentifySent => S("Das Display zeigt 10 s lang seinen Erkennungsbildschirm.", "The display shows its identify screen for 10 s.");
    public static string SelectDisplay => S("Bitte ein Display auswählen (Häkchen oder Zeile).", "Please select a display (check box or row).");

    // ── Options ───────────────────────────────────────────────────────────────
    public static string BusShare => S("Busanteil (%):", "Bus share (%):");
    public static string Adaptive => S("Bei Fremdlast zurücknehmen", "Back off under foreign load");
    public static string InstallFirmware => S("Firmware", "Firmware");
    public static string InstallWebUi => S("Weboberfläche", "Web interface");
    public static string Compress => S("Komprimiert übertragen", "Compressed transfer");
    public static string ApplyLabel => S("Aktivieren:", "Activate:");
    public static string ApplyNow => S("Sofort, mit Prüfung (empfohlen)", "Now, with check (recommended)");
    public static string ApplyNowNoWait => S("Sofort, ohne auf die Prüfung zu warten", "Now, without waiting for the check");
    public static string ApplyNext => S("Beim nächsten Neustart", "At the next restart");
    public static string ApplyNone => S("Nur übertragen und prüfen", "Transfer and verify only");
    public static string WebUiVersion => S("Version der Weboberfläche:", "Web interface version:");
    public static string ForceVersion => S("Versionsprüfung übergehen", "Skip the version check");
    public static string Language => S("English", "Deutsch");

    // ── Progress ──────────────────────────────────────────────────────────────
    public static string Idle => S("Bereit", "Ready");
    public static string StayOpen =>
        S("Nicht schließen: Das Display prüft die neue Firmware (ca. 1–2 min).",
          "Do not close: the display is checking the new firmware (about 1-2 min).");
    public static string Target(UpdateTarget t) => t == UpdateTarget.Firmware ? S("Firmware", "Firmware") : S("Weboberfläche", "Web interface");

    public static string Phase(UpdateProgress p) => p.Phase switch
    {
        SenderPhase.Offering => S("Angebot an das Display …", "Offering to the display …"),
        SenderPhase.WaitingForConfirmation => S($"Warte auf Bestätigung am Display ({p.DialogSecondsLeft ?? 60} s)",
                                                $"Waiting for confirmation on the display ({p.DialogSecondsLeft ?? 60} s)"),
        SenderPhase.Preparing => S("Display bereitet vor …", "Display preparing …"),
        SenderPhase.Transferring => S($"Übertragung {p.Fraction:P0}", $"Transfer {p.Fraction:P0}") +
                                    (p.EtaSeconds is { } e ? S($" – noch ca. {Minutes(e)}", $" – about {Minutes(e)} left") : ""),
        SenderPhase.PausedForeignTransfer => S("Pause: anderes Update am Bus", "Paused: another update on the bus"),
        SenderPhase.Verifying => S("Display prüft die Daten …", "Display verifying …"),
        SenderPhase.Applying => S("Aktivieren …", "Activating …"),
        SenderPhase.WaitingForValidation => S("Neustart und Prüfung der neuen Firmware", "Restart and check of the new firmware") +
                                            (p.VerifySecondsLeft is { } v ? S($" (spätestens {v} s)", $" (at most {v} s)") : ""),
        SenderPhase.CheckingWebUi => S("Prüfe die Weboberfläche …", "Checking the web interface …"),
        SenderPhase.Recovering => S("Verbindung wird wiederhergestellt …", "Recovering the connection …"),
        _ => S("Fertig", "Done"),
    };

    private static string Minutes(double s) => s < 90 ? S($"{s:F0} s", $"{s:F0} s") : S($"{s / 60:F0} min", $"{s / 60:F0} min");

    public static string Outcome(UpdateOutcome? o)
    {
        if (o is null) return "–";
        // What the sender itself found reads from the PC's side; a display reason from
        // the display's (its TIMEOUT means "connection to the sender lost"). The
        // library's English message only goes to the log.
        string why = Note(o.Note) ?? (o.Reason != Reason.Ok ? ReasonTexts.Describe(o.Reason, German) : "");
        string tail = why.Length > 0 ? ": " + why : "";
        return o.Kind switch
        {
            OutcomeKind.Success => S("Erfolgreich", "Successful"),
            OutcomeKind.Verified => S("Übertragen und geprüft (nicht aktiviert)", "Transferred and verified (not activated)"),
            OutcomeKind.InstalledRestartPostponed => S("Installiert, Neustart am Display verschoben", "Installed, restart postponed on the display"),
            OutcomeKind.InstalledNotConfirmed => S("Installiert", "Installed") + tail,
            OutcomeKind.RolledBack => RolledBack(o),
            OutcomeKind.Rejected => S("Abgelehnt", "Rejected") + tail,
            OutcomeKind.Aborted when o.Note == SenderNote.Cancelled => S("Am PC abgebrochen", "Cancelled on the PC"),
            OutcomeKind.Aborted => (o.ByDevice && o.Note == SenderNote.None ? S("Abgebrochen am Display", "Aborted on the display") : S("Abgebrochen", "Aborted")) + tail,
            OutcomeKind.Failed => S("Fehlgeschlagen", "Failed") + tail,
            OutcomeKind.Unknown => S("Ergebnis unbekannt", "Outcome unknown") + tail,
            OutcomeKind.Refused => S("Nicht gesendet", "Not sent") + tail,
            OutcomeKind.Skipped => S("Übersprungen", "Skipped") + tail,
            _ => o.ToString(),
        };
    }

    // 7.5: the display's own wording of each rollback cause (4.11).
    private static string RolledBack(UpdateOutcome o) => (byte)o.Cause switch
    {
        1 when o.Detail == 5 => S("Zurückgenommen: kein Empfang über NMEA 2000 nach dem Neustart", "Rolled back: no NMEA 2000 reception after the restart"),
        1 => S($"Zurückgenommen: Selbsttest nicht bestanden (Kriterium {o.Detail})", $"Rolled back: self-test failed (criterion {o.Detail})"),
        2 => S("Zurückgenommen: Absturz beim Start", "Rolled back: crashed while starting"),
        3 => S("Nicht bestätigt: Strom während der Prüfung unterbrochen. Bitte erneut installieren.", "Not confirmed: power was lost during the check. Please install it again."),
        4 => S("Nicht bestätigt: Neustart während der Prüfung", "Not confirmed: restarted during the check"),
        5 => S("Zurückgenommen: wiederholte Abstürze", "Reverted: repeated crashes"),
        _ => S("Zurückgenommen", "Rolled back"),
    };

    private static string? Note(SenderNote n) => n switch
    {
        SenderNote.DisplayRestarted => S("Das Display wurde während der Übertragung neu gestartet", "The display restarted during the transfer"),
        SenderNote.SessionDropped => S("Das Display hat die Übertragung beendet", "The display ended the transfer"),
        SenderNote.DisplayUnreachable => S("Display nicht mehr erreichbar", "The display could not be reached any more"),
        SenderNote.NoOfferReply => S("Keine Antwort vom Display", "No answer from the display"),
        SenderNote.NoResumeReply => S("Keine Antwort vom Display beim Fortsetzen", "No answer from the display when resuming"),
        SenderNote.AdapterGone => S("Der CAN-Adapter kam nicht zurück", "The CAN adapter did not come back"),
        SenderNote.NoResult => S("Kein Prüfergebnis vom Display", "No check result from the display"),
        SenderNote.NoDecision => S("Am Display nicht bestätigt", "Not confirmed on the display"),
        SenderNote.DialogClosed => S("Das Display hat die Abfrage geschlossen", "The display closed the dialog"),
        SenderNote.BusBusy => S("Ein anderes Update belegte den Bus", "Another update kept the bus busy"),
        SenderNote.ChunkRepeated => S("Bus zu unzuverlässig", "Bus too unreliable"),
        SenderNote.InternalError => S("Interner Fehler im Updater", "Internal error in the updater"),
        SenderNote.NoActivationConfirmation => S("Aktivierung nicht bestätigt", "Activation not confirmed"),
        SenderNote.PollingStopped => S("Abgebrochen, bevor das Display die Firmware bestätigt hat – es kehrt zur vorherigen zurück",
                                        "Stopped before the display confirmed the firmware – it returns to the previous one"),
        SenderNote.NoInfo => S("Das Display meldet sich nach dem Neustart nicht", "The display did not report back after the restart"),
        SenderNote.NoDecisionInTime => S("Keine Entscheidung innerhalb von 5 Minuten", "No decision within 5 minutes"),
        SenderNote.RestartedWithoutNewImage => S("Neu gestartet, aber ohne die neue Firmware", "Restarted, but not with the new firmware"),
        SenderNote.WebUiNotConfirmed => S("Neue Weboberfläche nicht bestätigt", "New web interface not confirmed"),
        SenderNote.LegacyImage => S("Firmware ohne N2K-Kennung – einmal über WLAN oder USB installieren",
                                     "Firmware without N2K descriptor – install it once over WiFi or USB"),
        SenderNote.BadImage => S("Datei ist kein gültiges Abbild", "The file is not a valid image"),
        SenderNote.WrongBoard => ReasonTexts.Describe(Reason.WrongBoard, German),
        SenderNote.TooLarge => ReasonTexts.Describe(Reason.TooLarge, German),
        SenderNote.IncompatibleProtocol => S("Update-Protokoll nicht kompatibel", "Update protocol not compatible"),
        SenderNote.NotSupported => S("Vom Display nicht unterstützt", "Not supported by the display"),
        SenderNote.NothingToInstall => S("Nichts zu installieren", "Nothing to install"),
        SenderNote.WebUiVersionUnknown => S("Version der Weboberfläche unbekannt – eintragen oder Versionsprüfung übergehen",
                                             "Web interface version unknown – enter it or skip the version check"),
        SenderNote.WebUiVersionMismatch => ReasonTexts.Describe(Reason.VersionMismatch, German),
        SenderNote.Cancelled => S("am PC abgebrochen", "cancelled on the PC"),
        SenderNote.FirmwareNotInstalled => S("Die Firmware wurde nicht installiert", "The firmware was not installed"),
        SenderNote.FirmwareActiveAfterRestart => S("Die Firmware wird erst nach dem nächsten Neustart aktiv; die Weboberfläche danach installieren",
                                                    "The firmware becomes active at the next restart; install the web interface after that"),
        SenderNote.WebUiAlreadyInstalled => S("Diese Weboberfläche ist schon installiert", "This web interface is already installed"),
        SenderNote.NotActivated => S("Nicht aktiviert", "Not activated"),
        SenderNote.ActiveFromNextRestart => S("aktiv ab dem nächsten Neustart", "active from the next restart"),
        SenderNote.RestartNotObserved => S("Neustart und Prüfung nicht abgewartet", "restart and check not awaited"),
        _ => null,
    };

    public static string Yes => S("Ja", "Yes");
    public static string No => S("Nein", "No");
    public static string Ok => S("OK", "OK");
    public static string AdapterLost => S("Verbindung zum CAN-Adapter verloren", "Connection to the CAN adapter lost");
    public static string IdentifyFailed => S("Nicht gesendet: CAN-Adapter nicht erreichbar", "Not sent: CAN adapter not reachable");

    // ── Dialogs ───────────────────────────────────────────────────────────────
    public static string ConfirmTitle => S("Installieren", "Install");
    public static string ConfirmIntro => S("Folgendes wird installiert:", "The following will be installed:");
    public static string ConfirmOutro =>
        S("Jedes Display fragt auf seinem Bildschirm nach einer Bestätigung (Installieren gedrückt halten). " +
          "Firmware und Weboberfläche eines Releases werden dort gemeinsam bestätigt.\n\nFortfahren?",
          "Each display asks for a confirmation on its screen (press and hold Install). " +
          "Firmware and web interface of one release are confirmed there together.\n\nContinue?");
    public static string NothingForDisplay => S("nichts Passendes im Release", "nothing suitable in the release");
    public static string CloseDuringCheckTitle => S("Display prüft die Firmware", "Display is checking the firmware");
    public static string CloseDuringCheck =>
        S("Das Display prüft gerade die neue Firmware und braucht dafür die Abfragen dieses Programms.\n\n" +
          "Wenn Sie jetzt schließen oder abbrechen, kehrt das Display in 1–3 Minuten zur vorherigen Firmware zurück.\n\nTrotzdem beenden?",
          "The display is checking the new firmware and needs this program's queries for that.\n\n" +
          "If you close or cancel now, the display returns to its previous firmware within 1-3 minutes.\n\nStop anyway?");
    public static string CloseDuringTransfer =>
        S("Ein Update läuft. Abbrechen und schließen?", "An update is running. Cancel it and close?");
    public static string Summary => S("Ergebnis", "Result");
}
