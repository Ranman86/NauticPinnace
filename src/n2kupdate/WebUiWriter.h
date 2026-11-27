#pragma once
// ============================================================================
// WebUiWriter - the web UI writer (docs/N2K-UPDATE.md 9.4), shared by the
// update over NMEA 2000 and the WiFi upload (/api/ota?target=webui).
//
// The web UI is ONE file, /index.html.gz. config.json, polar.json, the logo
// and the plain /index.html are never opened (R1).
//
//   begin    delete a stale /index.html.gz.new and open it for writing
//   write    append (strictly sequential, from the bounce buffer)
//   finish   close; reopen for reading
//   readBack read through the bounce buffer
//   apply    1. rename .new over /index.html.gz - atomic in LittleFS, but it
//               fails with EBUSY while either name is open (a slow client on
//               the fallback route): APPLY_ST_BUSY, the receiver retries every
//               100 ms for up to 10 s
//            2. reload the new file into the work region, 4 KB per call, and
//               hash it into the web UI id
//            3. publish the region as the live cache (one pointer store);
//               the old region gets its 120 s grace
//            detail 1 = the reload failed, the new file is served from the
//            next restart.
//   abort    close and delete the temporary file
// ============================================================================
#include "N2kUpdateCore.h"
#include "UpdateGate.h"

#ifndef SIMULATOR

class WebUiWriter : public n2kupd::IUpdTarget {
public:
    // `reloadSha` hashes the reload in apply(). The N2K writer borrows the
    // receiver's SHA object: the receiver is finished with it by APPLY.
    WebUiWriter(UpdateGate::Owner owner, n2kupd::ISha256 *reloadSha)
        : _owner(owner), _sha(reloadSha) {}

    // ---- n2kupd::IUpdTarget ---------------------------------------------------------
    uint8_t begin(uint32_t imageSize) override;
    uint8_t write(uint32_t offset, const uint8_t *bounce, size_t n) override;
    uint8_t finish() override;
    uint8_t readBack(uint32_t offset, uint8_t *bounce, size_t n) override;
    n2kupd::ApplyStatus apply(const n2kupd::ApplyInfo &info, uint8_t *detail) override;
    void abort() override;

    // ---- WiFi upload ------------------------------------------------------------------
    // Takes the work region (the reload needs it) and then begin()s. A region
    // still in its grace after the previous swap: RSN_BUSY and `retryMs`.
    uint8_t beginWifi(uint32_t &retryMs);
    // Any source buffer, copied through the bounce buffer. The first bytes must
    // be the gzip magic. Returns a reason code; aborts itself on failure.
    uint8_t writeAny(const uint8_t *src, size_t n);
    // finish, read back and hash, rename (retried for up to 2 s - this runs in
    // the web server task), reload and publish in one go. `detail` as in
    // RESULT phase 2: 1 = served from the next restart.
    uint8_t finishAndApply(uint8_t &detail);
    uint32_t written() const { return _written; }

private:
    enum Stage : uint8_t { ST_RENAME = 0, ST_RELOAD, ST_DONE };
    void release();
    void closeFile();

    UpdateGate::Owner _owner;
    n2kupd::ISha256  *_sha;
    int      _fd = -1;                  // UpdFile descriptor (unbuffered, 9.1)
    bool     _claimed = false;
    bool     _ownsWork = false;         // WiFi: the work region was taken by beginWifi()
    bool     _writing = false;          // .new exists and belongs to this session
    uint32_t _written = 0;
    uint32_t _readPos = 0;
    // apply
    uint8_t  _stage = ST_RENAME;
    uint32_t _reloadPos = 0, _reloadLen = 0;
    bool     _reloadFailed = false;
};
#endif
