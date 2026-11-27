// ============================================================================
// WebUiWriter.cpp - see WebUiWriter.h.
// ============================================================================
#ifndef SIMULATOR
#include "WebUiWriter.h"
#include "UpdRegions.h"
#include "UpdCrypto.h"
#include "UpdFile.h"
#include "OtaGuard.h"
#include <Arduino.h>
#include <errno.h>
#include <string.h>

using namespace n2kupd;

// Every file access goes through UpdFile (unbuffered VFS calls), never
// through Arduino's File: its 4 KB stdio buffer would break the bounce rule
// (9.1). UpdFile's rename also reports EBUSY (a name still open), the one
// failure worth waiting for, which LittleFS.rename() hides behind a bool.
static const char *const PATH_NEW  = "/index.html.gz.new";
static const char *const PATH_LIVE = "/index.html.gz";

static const uint8_t GZIP_MAGIC[3] = { 0x1F, 0x8B, 0x08 };

void WebUiWriter::closeFile() {
    if (_fd >= 0) {
        UpdFile::closeFd(_fd);
        _fd = -1;
    }
}

void WebUiWriter::release() {
    if (_ownsWork) {
        UpdRegions::releaseWork();
        _ownsWork = false;
    }
    if (_claimed) {
        UpdateGate::releaseWriter(_owner);
        _claimed = false;
    }
}

uint8_t WebUiWriter::begin(uint32_t imageSize) {
    (void)imageSize;                  // space was checked with the OFFER (5.3.1 step 9)
    closeFile();
    if (_writing) UpdFile::removeFile(PATH_NEW);
    _writing = false;
    if (!UpdateGate::claimWriter(_owner)) {
        Serial.println("[web] writer busy - another update owns it");
        return RSN_INTERNAL_ERROR;
    }
    _claimed = true;
    if (UpdFile::exists(PATH_NEW)) UpdFile::removeFile(PATH_NEW);
    _fd = UpdFile::openWrite(PATH_NEW);
    if (_fd < 0) {
        Serial.println("[web] cannot create /index.html.gz.new");
        release();
        return RSN_FLASH_ERROR;
    }
    _writing = true;
    _written = 0;
    _readPos = 0;
    _stage = ST_RENAME;
    _reloadPos = _reloadLen = 0;
    _reloadFailed = false;
    return RSN_OK;
}

uint8_t WebUiWriter::write(uint32_t offset, const uint8_t *bounce, size_t n) {
    if (_fd < 0 || !_writing || offset != _written || n == 0 || n > BOUNCE_BYTES)
        return RSN_INTERNAL_ERROR;
    if (!UpdFile::writeAll(_fd, bounce, n)) return RSN_FLASH_ERROR;
    _written += (uint32_t)n;
    return RSN_OK;
}

uint8_t WebUiWriter::finish() {
    if (!_writing) return RSN_INTERNAL_ERROR;
    closeFile();
    if (UpdFile::fileSize(PATH_NEW) != (int32_t)_written) return RSN_FLASH_ERROR;
    _fd = UpdFile::openRead(PATH_NEW);
    if (_fd < 0) return RSN_FLASH_ERROR;
    _readPos = 0;
    return RSN_OK;
}

uint8_t WebUiWriter::readBack(uint32_t offset, uint8_t *bounce, size_t n) {
    if (_fd < 0 || n > BOUNCE_BYTES) return RSN_FLASH_ERROR;
    if (offset != _readPos) {
        if (!UpdFile::seekTo(_fd, offset)) return RSN_FLASH_ERROR;
        _readPos = offset;
    }
    if (!UpdFile::readExact(_fd, bounce, n)) return RSN_FLASH_ERROR;
    _readPos += (uint32_t)n;
    return RSN_OK;
}

ApplyStatus WebUiWriter::apply(const ApplyInfo &info, uint8_t *detail) {
    *detail = 0;
    switch (_stage) {
        case ST_RENAME: {
            if (!_writing) return APPLY_ST_ERROR;
            closeFile();              // our own read handle would block the rename
            const int err = UpdFile::renameFile(PATH_NEW, PATH_LIVE);
            if (err) {
                if (err == EBUSY) return APPLY_ST_BUSY;   // a response holds a name open
                Serial.printf("[web] rename failed (errno %d)\n", err);
                return APPLY_ST_ERROR;
            }
            // From here on the new file IS the stored web UI; nothing to delete.
            _writing = false;
            _stage = ST_RELOAD;
            _reloadPos = 0;
            _reloadLen = info.imageSize;
            _reloadFailed = !UpdRegions::workBase() || !_sha ||
                            _reloadLen > UpdRegions::REGION_BYTES;
            if (!_reloadFailed) {
                _fd = UpdFile::openRead(PATH_LIVE);
                if (_fd < 0) _reloadFailed = true;
                else _sha->init();
            }
            return APPLY_ST_AGAIN;
        }
        case ST_RELOAD: {
            // 4 KB per call into the work region, through the bounce buffer:
            // LittleFS reads large aligned blocks straight into the caller's
            // buffer, which must never be PSRAM (9.1).
            if (!_reloadFailed) {
                uint8_t *dst = UpdRegions::workBase();
                uint8_t *b = UpdRegions::bounce();
                uint32_t stop = _reloadPos + FLASH_UNIT;
                if (stop > _reloadLen) stop = _reloadLen;
                while (_reloadPos < stop) {
                    const size_t n = (stop - _reloadPos) < BOUNCE_BYTES ? (stop - _reloadPos)
                                                                       : BOUNCE_BYTES;
                    if (!dst || !UpdFile::readExact(_fd, b, n)) { _reloadFailed = true; break; }
                    _sha->update(b, n);
                    memcpy(dst + _reloadPos, b, n);
                    _reloadPos += (uint32_t)n;
                }
                if (!_reloadFailed && _reloadPos < _reloadLen) return APPLY_ST_AGAIN;
            }
            closeFile();
            if (!_reloadFailed) {
                uint8_t sha[32];
                _sha->finish(sha);
                // The cache must hold exactly the verified file.
                if (memcmp(sha, info.sha256, 32) != 0 || !UpdRegions::publishWork(_reloadLen))
                    _reloadFailed = true;
            }
            // The id is the file's SHA-256 - the read-back value of 5.5 - so it
            // is right even when the cache could not be reloaded.
            UpdRegions::setWebUiIdentity(info.sha256, info.imageSize, info.version);
            if (info.viaCompanion) OtaGuard::companionConsumed();   // 5.3.6 step 4
            *detail = _reloadFailed ? 1 : 0;
            _stage = ST_DONE;
            Serial.printf("[web] web interface replaced (%u bytes)%s\n", (unsigned)info.imageSize,
                          _reloadFailed ? " - served from the next restart" : "");
            release();
            return APPLY_ST_DONE;
        }
        default:
            return APPLY_ST_ERROR;
    }
}

void WebUiWriter::abort() {
    closeFile();
    if (_writing) UpdFile::removeFile(PATH_NEW);
    _writing = false;
    _stage = ST_RENAME;
    release();
}

// ---- WiFi upload ----------------------------------------------------------------------

uint8_t WebUiWriter::beginWifi(uint32_t &retryMs) {
    retryMs = 0;
    WorkArea wa;
    const uint32_t wait = UpdRegions::acquireWork(wa);
    if (wait) { retryMs = wait; return RSN_BUSY; }
    if (!wa.window) return RSN_OUT_OF_MEMORY;
    _ownsWork = true;
    const uint8_t r = begin(0);
    if (r) release();
    return r;
}

uint8_t WebUiWriter::writeAny(const uint8_t *src, size_t n) {
    uint8_t *b = UpdRegions::bounce();
    while (n) {
        size_t piece = BOUNCE_BYTES - (_written % BOUNCE_BYTES);
        if (piece > n) piece = n;
        memcpy(b, src, piece);
        uint8_t r = RSN_OK;
        for (size_t i = 0; i < piece && _written + i < sizeof(GZIP_MAGIC); i++)
            if (b[i] != GZIP_MAGIC[_written + i]) r = RSN_IMAGE_INVALID;
        if (!r && _written + piece > UpdRegions::REGION_BYTES) r = RSN_TOO_LARGE;
        if (!r) r = write(_written, b, piece);
        if (r) { abort(); return r; }
        src += piece;
        n -= piece;
    }
    return RSN_OK;
}

uint8_t WebUiWriter::finishAndApply(uint8_t &detail) {
    detail = 0;
    if (_written < sizeof(GZIP_MAGIC)) { abort(); return RSN_SIZE_MISMATCH; }
    uint8_t r = finish();
    // Read back and hash: the web UI id is computed from what is in flash.
    ApplyInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.target = TARGET_WEBUI;
    ai.mode = APPLY_NOW;
    ai.imageSize = _written;
    if (!r) {
        MbedSha256 h;
        h.init();
        uint8_t *b = UpdRegions::bounce();
        for (uint32_t pos = 0; pos < _written && !r; ) {
            const size_t n = (_written - pos) < BOUNCE_BYTES ? (_written - pos) : BOUNCE_BYTES;
            r = readBack(pos, b, n);
            if (!r) { h.update(b, n); pos += (uint32_t)n; }
        }
        h.finish(ai.sha256);
    }
    if (r) { abort(); return r; }
    // The reload hash needs a SHA object; the WiFi writer has none of its own
    // (internal RAM), so it lends one from this stack for the apply below.
    MbedSha256 reloadSha;
    ISha256 *const saved = _sha;
    if (!_sha) _sha = &reloadSha;
    // No OFFER over WiFi, so no version: INFO reports the web UI version empty
    // and the sender goes by the web UI id, as it does anyway (5.7).
    uint8_t result = RSN_APPLY_FAILED;
    for (int tries = 0; ; ) {
        const ApplyStatus st = apply(ai, &detail);
        if (st == APPLY_ST_DONE) { result = RSN_OK; break; }
        if (st == APPLY_ST_AGAIN) continue;
        if (st == APPLY_ST_BUSY && ++tries <= 20) { delay(100); continue; }
        abort();
        break;
    }
    _sha = saved;
    return result;
}
#endif  // SIMULATOR
