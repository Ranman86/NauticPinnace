// strings_update.inc — updates over NMEA 2000 (docs/N2K-UPDATE.md 7.x, 4.8):
// the confirmation dialog, the progress banner, the restart countdown, the
// notices after a restart, the identify screen and the reason texts.
//
// One line per string:  X(KEY, "deutsch", "english")
// Rules:
//   - KEY is unique across ALL .inc files (they share one enum). Prefixed UPD_
//     here, RSN_ for the reason texts of table 4.8.
//   - NO printf specifiers (house rule): numbers, versions and names are
//     appended in code.
//   - Only glyphs the panel fonts have: umlauts, ß, the en dash and the
//     typographic quotes are fine - no arrows, no ellipsis, no middle dot.
//   - Write real UTF-8. Do NOT use \xNN escapes (they are greedy).
#define I18N_STRINGS_UPDATE(X) \
    /* ── Confirmation dialog (7.2) ───────────────────────────────────────── */ \
    X(UPD_TITLE,           "Update über NMEA 2000",     "Update over NMEA 2000")  \
    X(UPD_FROM,            "Von",                       "From")                   \
    X(UPD_ADDRESS,         "Adresse",                   "address")                \
    X(UPD_CONTENT,         "Inhalt",                    "Content")                \
    X(UPD_FIRMWARE,        "Firmware",                  "Firmware")               \
    X(UPD_WEBUI,           "Weboberfläche",             "Web interface")          \
    X(UPD_FW_PLUS_WEB,     "Firmware + Weboberfläche",  "Firmware + web interface") \
    X(UPD_DEVICE,          "Gerät",                     "Device")                 \
    X(UPD_INSTALLED,       "Installiert",               "Installed")              \
    X(UPD_NEW,             "Neu",                       "New")                    \
    X(UPD_OLDER,           "(älter)",                   "(older)")                \
    X(UPD_SAME_VERSION,    "(gleiche Version)",         "(same version)")         \
    X(UPD_SIZE,            "Größe",                     "Size")                   \
    X(UPD_TRANSFERRED,     "übertragen",                "transferred")            \
    X(UPD_DURATION,        "Dauer ca.",                 "Takes about")            \
    X(UPD_NOTE_FIRMWARE,   "Die Anzeige bleibt währenddessen nutzbar. Danach startet das Gerät neu (ca. 30 s).", \
                           "The display stays usable meanwhile. Afterwards the device restarts (about 30 s).") \
    X(UPD_NOTE_WEBUI,      "Die Anzeige bleibt nutzbar, kein Neustart.",          \
                           "The display stays usable; no restart.")              \
    X(UPD_NOTE_ANCHOR,     "Ankerwache aktiv: Während des Neustarts gibt dieses Gerät keinen Ankeralarm.", \
                           "Anchor watch armed: this display gives no anchor alarm while it restarts.") \
    X(UPD_AUTO_DECLINE,    "Automatische Ablehnung in", "Declined automatically in") \
    X(UPD_INSTALL_HOLD,    "Installieren (halten)",     "Install (hold)")         \
    X(UPD_DECLINE,         "Ablehnen",                  "Decline")                \
    /* ── Progress banner (7.3) ───────────────────────────────────────────── */ \
    X(UPD_UPDATE,          "Update",                    "Update")                 \
    X(UPD_REMAINING,       "ca.",                       "approx.")                \
    X(UPD_PREPARING,       "Update wird vorbereitet",   "Preparing update")       \
    X(UPD_CHECKING,        "Update wird geprüft",       "Checking update")        \
    X(UPD_ACTIVATING,      "Update wird aktiviert",     "Activating update")      \
    X(UPD_CANCEL_HOLD,     "Abbrechen (halten)",        "Cancel (hold)")          \
    /* ── Restart countdown (7.4) ─────────────────────────────────────────── */ \
    X(UPD_READY_RESTART,   "Update bereit. Neustart in", "Update ready. Restarting in") \
    X(UPD_RESTART_NOW,     "Jetzt neu starten",         "Restart now")            \
    X(UPD_LATER,           "Später",                    "Later")                  \
    X(UPD_ACTIVE_NEXT,     "Update wird beim nächsten Neustart aktiv.",           \
                           "The update becomes active at the next restart.")     \
    X(UPD_RESTART_AFTER_ALARM, "Neustart nach dem Alarm", "Restart after the alarm") \
    /* ── After the restart (7.5) ─────────────────────────────────────────── */ \
    X(UPD_SUCCESS,         "Update erfolgreich:",       "Update successful:")     \
    X(UPD_RB_FAILED,       "Update fehlgeschlagen:",    "Update failed:")         \
    X(UPD_RB_NO_RX,        "kein Empfang über NMEA 2000", "no NMEA 2000 reception") \
    X(UPD_RB_SELFTEST,     "Selbsttest nicht bestanden", "self-test failed")      \
    X(UPD_RB_CRASH,        "Absturz beim Start",        "crashed while starting") \
    X(UPD_RB_NOT_CONFIRMED,"Update nicht bestätigt:",   "Update not confirmed:")  \
    X(UPD_RB_POWER,        "Strom während der Prüfung unterbrochen. Bitte erneut installieren.", \
                           "power was lost during the check. Please install it again.") \
    X(UPD_RB_RESTART,      "Neustart während der Prüfung", "restarted during the check") \
    X(UPD_RB_REVERTED,     "Update zurückgenommen:",    "Update reverted:")       \
    X(UPD_RB_CRASHES,      "wiederholte Abstürze",      "repeated crashes")       \
    X(UPD_RUNNING_AGAIN,   "Wieder aktiv:",             "Running again:")         \
    X(UPD_OK,              "OK",                        "OK")                     \
    /* ── Failure during a session, web interface (7.6) ───────────────────── */ \
    X(UPD_ABORTED,         "Update abgebrochen:",       "Update aborted:")        \
    X(UPD_ERROR,           "Fehler",                    "Error")                  \
    X(UPD_WEB_UPDATED,     "Weboberfläche aktualisiert.", "Web interface updated.") \
    X(UPD_WEB_NEXT_RESTART,"Neue Weboberfläche ab dem nächsten Neustart.",        \
                           "New web interface from the next restart.")           \
    /* ── Identify screen (7.7) ───────────────────────────────────────────── */ \
    X(UPD_THIS_DISPLAY,    "Dieses Display",            "This display")           \
    /* ── Postponed restart and demo mode (7.8) ───────────────────────────── */ \
    X(UPD_RESTART_AFTER_UPDATE, "Neustart erst nach dem Update", "Restart after the update") \
    X(UPD_DEMO_AFTER_UPDATE,    "Demo-Modus erst nach dem Update", "Demo mode after the update") \
    /* ── Setting (7.9) - short: it shares a row on the 480 px panel ──────── */ \
    X(CFG_N2K_UPDATES,     "Updates über N2K",          "N2K updates")            \
    /* ── Reason texts, table 4.8 (0x0D and 0x19 are never shown) ─────────── */ \
    X(RSN_DECLINED_BY_USER,     "Am Display abgelehnt",       "Declined on the display")     \
    X(RSN_CONFIRM_TIMEOUT,      "Am Display nicht bestätigt", "Not confirmed on the display") \
    X(RSN_DISABLED,             "Updates über NMEA 2000 sind ausgeschaltet",            \
                                "Updates over NMEA 2000 are switched off")             \
    X(RSN_BUSY,                 "Ein anderes Update läuft",   "Another update is in progress") \
    X(RSN_WRONG_BOARD,          "Firmware für ein anderes Gerät", "Firmware for a different device") \
    X(RSN_TOO_LARGE,            "Datei zu groß",              "File too large")      \
    X(RSN_UNSUPPORTED,          "Nicht unterstützt",          "Not supported")       \
    X(RSN_BAD_REQUEST,          "Ungültige Anfrage",          "Invalid request")     \
    X(RSN_PENDING_VERIFY,       "Das letzte Update wird noch geprüft",                  \
                                "The previous update is still being checked")          \
    X(RSN_ALARM_ACTIVE,         "Alarm aktiv",                "Alarm active")        \
    X(RSN_VERSION_MISMATCH,     "Weboberfläche passt nicht zur Firmware",               \
                                "Web interface does not match the firmware")           \
    X(RSN_NO_SPACE,             "Zu wenig Platz im Dateisystem", "Not enough space in the file system") \
    X(RSN_OUT_OF_MEMORY,        "Zu wenig Arbeitsspeicher",   "Out of memory")       \
    X(RSN_DESCRIPTOR_INVALID,   "Firmware ohne gültige Kennung", "Firmware without a valid identification") \
    X(RSN_SHA_MISMATCH,         "Prüfsumme falsch – Daten beschädigt", "Checksum mismatch – data corrupted") \
    X(RSN_IMAGE_INVALID,        "Firmware-Abbild ungültig",   "Firmware image invalid") \
    X(RSN_SIZE_MISMATCH,        "Größe stimmt nicht",         "Size mismatch")       \
    X(RSN_FLASH_ERROR,          "Fehler beim Schreiben",      "Write error")         \
    X(RSN_TIMEOUT,              "Verbindung zum Absender verloren", "Connection to the sender lost") \
    X(RSN_USER_CANCELLED,       "Am Display abgebrochen",     "Cancelled on the display") \
    X(RSN_SENDER_CANCELLED,     "Vom Absender abgebrochen",   "Cancelled by the sender") \
    X(RSN_STREAM_CRC_MISMATCH,  "Übertragungsfehler",         "Transmission error")  \
    X(RSN_LINK_UNRELIABLE,      "Bus zu unzuverlässig",       "Bus too unreliable")  \
    X(RSN_APPLY_FAILED,         "Aktivieren fehlgeschlagen",  "Activation failed")   \
    X(RSN_RESTART_PENDING,      "Neustart ausstehend",        "Restart pending")     \
    X(RSN_NOT_READY,            "Gerät startet noch",         "Device is still starting") \
    X(RSN_INTERNAL_ERROR,       "Interner Fehler",            "Internal error")
