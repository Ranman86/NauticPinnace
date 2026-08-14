#include "I18n.h"
#include <string.h>

// Two flat tables of pointers into flash. Index == StrId, so a lookup is a
// single load — cheap enough to call from inside a per-frame update().
static const char *const S_DE[] = {
#define X(k, de, en) de,
    I18N_STRINGS(X)
#undef X
};
static const char *const S_EN[] = {
#define X(k, de, en) en,
    I18N_STRINGS(X)
#undef X
};

static_assert(sizeof(S_DE) / sizeof(S_DE[0]) == STR__COUNT, "DE table size mismatch");
static_assert(sizeof(S_EN) / sizeof(S_EN[0]) == STR__COUNT, "EN table size mismatch");

static Lang s_lang = Lang::DE;

const char *TL(StrId id, Lang l) {
    if ((uint16_t)id >= STR__COUNT) return "?";
    return (l == Lang::EN ? S_EN : S_DE)[id];
}

const char *T(StrId id) { return TL(id, s_lang); }

Lang i18nLang()            { return s_lang; }
void i18nSetLang(Lang l)   { s_lang = l; }

Lang i18nLangFromCode(const char *code) {
    if (code && (code[0] == 'e' || code[0] == 'E') &&
                (code[1] == 'n' || code[1] == 'N')) return Lang::EN;
    return Lang::DE;
}

const char *i18nLangCode(Lang l) { return l == Lang::EN ? "en" : "de"; }

// Mirrors the FIELDS table in data/index.html — same ids, same wording.
const char *i18nFieldName(const char *key) {
    if (!key || !key[0]) return "";
    struct FieldName { const char *key; StrId id; };
    static const FieldName K[] = {
        {"sog", STR_FLD_SOG}, {"cog", STR_FLD_COG}, {"hdg", STR_FLD_HDG},
        {"stw", STR_FLD_STW}, {"awa", STR_FLD_AWA}, {"aws", STR_FLD_AWS},
        {"twa", STR_FLD_TWA}, {"tws", STR_FLD_TWS}, {"twd", STR_FLD_TWD},
        {"depth", STR_FLD_DEPTH}, {"rpm", STR_FLD_RPM}, {"oil", STR_FLD_OIL},
        {"coolant", STR_FLD_COOLANT}, {"hours", STR_FLD_HOURS},
        {"fuel", STR_FLD_FUEL}, {"rudder", STR_FLD_RUDDER},
        {"battv", STR_FLD_BATTV}, {"lat", STR_FLD_LAT}, {"lon", STR_FLD_LON},
        {"aptarget", STR_FLD_APTARGET},
    };
    for (const FieldName &f : K)
        if (strcmp(f.key, key) == 0) return T(f.id);
    return key;   // unknown id: show it raw rather than nothing
}
