// ============================================================================
// UpdNvs.cpp - see UpdNvs.h.
// ============================================================================
#ifndef SIMULATOR
#include "UpdNvs.h"
#include <Preferences.h>

namespace UpdNvs {
namespace {

const char *NS = "n2kupd";

bool load(const char *key, void *p, size_t n) {
    Preferences pr;
    if (!pr.begin(NS, true)) return false;      // read-only: absent on first boot
    // isKey() first: most of these records are normally absent ("clg" and
    // "preweb" are read on every boot), and getBytesLength() of an absent key
    // logs "[E] ... nvs_get_blob len fail: <key> NOT_FOUND" - an error line in
    // every boot log for nothing. isKey() asks without logging (both cores).
    const bool ok = pr.isKey(key) && pr.getBytesLength(key) == n && pr.getBytes(key, p, n) == n;
    pr.end();
    return ok;
}

bool store(const char *key, const void *p, size_t n) {
    Preferences pr;
    if (!pr.begin(NS, false)) return false;
    const bool ok = pr.putBytes(key, p, n) == n;
    pr.end();
    return ok;
}

void erase(const char *key) {
    Preferences pr;
    if (!pr.begin(NS, false)) return;
    if (pr.isKey(key)) pr.remove(key);
    pr.end();
}

}  // namespace

bool loadH5(H5 &r)                { return load("h5", &r, sizeof(r)); }
bool storeH5(const H5 &r)         { return store("h5", &r, sizeof(r)); }
void clearH5()                    { erase("h5"); }
bool loadPreWeb(PreWeb &r)        { return load("preweb", &r, sizeof(r)); }
bool storePreWeb(const PreWeb &r) { return store("preweb", &r, sizeof(r)); }
void clearPreWeb()                { erase("preweb"); }
bool loadWebVer(WebVer &r)        { return load("webver", &r, sizeof(r)); }
bool storeWebVer(const WebVer &r) { return store("webver", &r, sizeof(r)); }
bool loadCause(Cause &r)          { return load("cause", &r, sizeof(r)); }
bool storeCause(const Cause &r)   { return store("cause", &r, sizeof(r)); }
void clearCause()                 { erase("cause"); }
bool loadRbInfo(RbInfo &r)        { return load("rbinfo", &r, sizeof(r)); }
bool storeRbInfo(const RbInfo &r) { return store("rbinfo", &r, sizeof(r)); }
void clearRbInfo()                { erase("rbinfo"); }
bool loadRbSeen(uint8_t id[8])    { return load("rbseen", id, 8); }
bool storeRbSeen(const uint8_t id[8]) { return store("rbseen", id, 8); }
void clearRbSeen()                { erase("rbseen"); }
bool loadClg(Clg &r)              { return load("clg", &r, sizeof(r)); }
bool storeClg(const Clg &r)       { return store("clg", &r, sizeof(r)); }
void clearClg()                   { erase("clg"); }
bool slotDirty()                  { uint8_t v = 0; return load("dirty", &v, 1) && v == 1; }
bool setSlotDirty()               { const uint8_t v = 1; return store("dirty", &v, 1); }
void clearSlotDirty()             { erase("dirty"); }

}  // namespace UpdNvs
#endif  // SIMULATOR
