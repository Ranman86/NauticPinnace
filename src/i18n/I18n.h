#pragma once
// ============================================================================
// I18n — user-visible strings in German and English.
//
// HOW IT WORKS
//   Every string is declared once as  X(KEY, "deutsch", "english")  in one of
//   the strings_*.inc fragments. Those expand into
//     - an enum  STR_<KEY>
//     - one const char* table per language, both in flash
//   and T(STR_<KEY>) returns the entry for the active language.
//
// HOW TO ADD A STRING
//   Put the X(...) line in the .inc file that owns that part of the UI, then
//   use T(STR_YOURKEY) at the call site. Nothing else to register.
//
// WHAT NOT TO PUT IN HERE
//   - JSON/config keys, NVS keys, HTTP routes, format specifiers ("%.1f"),
//     LV_SYMBOL_* — translating those breaks the config file or the protocol.
//   - Licence texts. A licence is only the licence in its original wording.
//   - Values that come off the NMEA 2000 bus (radio source names, waypoint
//     names, AIS ship names) — that is remote data, not UI text.
//   - User-editable labels stored in config.json (engine fields, tank names).
//     Only their DEFAULTS may be language-dependent.
//
// FONT NOTE
//   The panel renders with Montserrat + the generated latin_suppl_* fallback,
//   which together cover ASCII, umlauts, sharp-s, en dash, quotes, degree, ±, ×
//   and the LV_SYMBOL icons. Anything outside that set draws as an empty box.
// ============================================================================
#include <stdint.h>

#include "strings_core.inc"
#include "strings_screens.inc"
#include "strings_config.inc"

#define I18N_STRINGS(X) \
    I18N_STRINGS_CORE(X)    \
    I18N_STRINGS_SCREENS(X) \
    I18N_STRINGS_CONFIG(X)

enum StrId : uint16_t {
#define X(k, de, en) STR_##k,
    I18N_STRINGS(X)
#undef X
    STR__COUNT
};

enum class Lang : uint8_t { DE = 0, EN = 1 };
static constexpr uint8_t LANG_COUNT = 2;

// Active-language lookup. Never returns nullptr; an out-of-range id yields "?"
// so a bad call shows up on screen instead of crashing the render.
const char *T(StrId id);

// Translation in a specific language, regardless of the active one. Used by the
// WebUI, which may be viewed in a different language than the panel.
const char *TL(StrId id, Lang l);

Lang        i18nLang();
void        i18nSetLang(Lang l);

// "de" / "en" — the form stored in config.json and sent to the browser.
// Unknown codes fall back to German rather than failing.
Lang        i18nLangFromCode(const char *code);
const char *i18nLangCode(Lang l);

// Standard name of a data field, looked up by the id stored in config.json
// (GridCell::pgn — "oil", "coolant", "depth", …). Used by the engine cards and
// the data grid ONLY when the user has not entered a label of their own; a
// user-typed label is their data and is never translated.
// Returns the key itself for ids we don't know, so nothing ever renders blank.
const char *i18nFieldName(const char *key);
