// ============================================================================
// OtaWriter.cpp - see OtaWriter.h.
// ============================================================================
#ifndef SIMULATOR
#include "OtaWriter.h"
#include "UpdRegions.h"
#include "UpdNvs.h"
#include "OtaGuard.h"
#include "../Version.h"
#include "../BoardConfig.h"
#include <Arduino.h>
#include <string.h>

using namespace n2kupd;

// The three FW_BOARD_NAME strings (Version.h), in board id order 1..3. Only
// used for legacy images, which carry exactly one of them and no descriptor.
// (This table puts all three into every new image, so the "exactly one"
// rule of 8.6 must never be applied to an image that has a descriptor.)
static const char *const BOARD_NAMES[3] = {
    "waveshare-4inch-480x480", "waveshare-7b-1024x600", "waveshare-5b-1024x600",
};

// millis() of the last activation (esp_ota_set_boot_partition) in this boot.
static bool     s_activated   = false;
static uint32_t s_activatedMs = 0;

bool OtaWriter::activatedAt(uint32_t &ms) {
    ms = s_activatedMs;
    return s_activated;
}

static void noteActivated() {
    s_activatedMs = millis();
    s_activated = true;
    // The slot is now the one set to boot: no longer "being written" (10.5).
    UpdNvs::clearSlotDirty();
}

void OtaWriter::release() {
    if (_claimed) {
        UpdateGate::releaseWriter(_owner);
        _claimed = false;
    }
}

uint8_t OtaWriter::begin(uint32_t imageSize) {
    (void)imageSize;                  // OTA_WITH_SEQUENTIAL_WRITES: size not needed
    abort();                          // a leftover handle of an earlier attempt
    if (!UpdateGate::claimWriter(_owner)) {
        Serial.println("[ota] writer busy - another update owns it");
        return RSN_INTERNAL_ERROR;    // BUSY is an OFFER reply, not an ABORT reason
    }
    _claimed = true;
    _part = esp_ota_get_next_update_partition(nullptr);
    if (!_part) { release(); return RSN_INTERNAL_ERROR; }
    const esp_err_t e = esp_ota_begin(_part, OTA_WITH_SEQUENTIAL_WRITES, &_h);
    if (e != ESP_OK) {
        Serial.printf("[ota] esp_ota_begin failed: %s\n", esp_err_to_name(e));
        release();
        return RSN_FLASH_ERROR;
    }
    _open = true;
    // The slot no longer holds the image a rollback report may describe, and
    // until it is activated or erased again, what it holds was never started
    // (OtaGuard's rollback report must not mistake it for a rolled-back image
    // after a power loss mid-write).
    // ORDER MATTERS: the dirty mark first, the report deleted from NVS only
    // once the mark is stored. Nothing is written yet, so the slot still holds
    // the rolled-back image: with the report gone and no mark - a power cut
    // between the two, or a mark that could not be stored and then an abort
    // before the first write, which erases nothing - the next boot would
    // report that image as a new rollback, its cause taken from the reset.
    // The report in RAM ends in both cases (otherSlotOverwritten()): this
    // boot must not name the old image while its slot is being overwritten.
    const bool marked = UpdNvs::setSlotDirty();
    OtaGuard::otherSlotOverwritten(marked);
    _ended = false;
    _written = 0;
    _descChecked = false;
    _legacy = false;
    memset(_nameMatch, 0, sizeof(_nameMatch));
    memset(_nameSeen, 0, sizeof(_nameSeen));
    Serial.printf("[ota] writing slot '%s' @ 0x%06X\n", _part->label, (unsigned)_part->address);
    return RSN_OK;
}

uint8_t OtaWriter::write(uint32_t offset, const uint8_t *bounce, size_t n) {
    if (!_open || offset != _written || n == 0 || n > BOUNCE_BYTES) return RSN_INTERNAL_ERROR;
    // The IDF erases the sector a write enters (one sector per crossing), so
    // a 256-byte write is at most one erase plus four 64-byte page programs.
    const esp_err_t e = esp_ota_write(_h, bounce, n);
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) return RSN_IMAGE_INVALID;   // no 0xE9 at offset 0
    if (e != ESP_OK) {
        Serial.printf("[ota] esp_ota_write @%u failed: %s\n", (unsigned)offset, esp_err_to_name(e));
        return RSN_FLASH_ERROR;
    }
    _written += (uint32_t)n;
    return RSN_OK;
}

uint8_t OtaWriter::finish() {
    if (!_open) return _ended ? (uint8_t)RSN_OK : (uint8_t)RSN_INTERNAL_ERROR;
    // esp_ota_end validates the whole image (structure, appended SHA-256) and
    // frees the handle whatever the outcome.
    const esp_err_t e = esp_ota_end(_h);
    _open = false;
    if (e != ESP_OK) {
        Serial.printf("[ota] esp_ota_end failed: %s\n", esp_err_to_name(e));
        return RSN_IMAGE_INVALID;
    }
    _ended = true;
    return RSN_OK;
}

uint8_t OtaWriter::readBack(uint32_t offset, uint8_t *bounce, size_t n) {
    if (!_part || n > BOUNCE_BYTES || offset + n > _part->size) return RSN_FLASH_ERROR;
    return esp_partition_read(_part, offset, bounce, n) == ESP_OK ? (uint8_t)RSN_OK
                                                                  : (uint8_t)RSN_FLASH_ERROR;
}

ApplyStatus OtaWriter::apply(const ApplyInfo &info, uint8_t *detail) {
    *detail = 0;
    if (!_ended || !_part) return APPLY_ST_ERROR;

    // The H5 marker and the companion approval go to NVS BEFORE the boot
    // partition is set (5.6 has them after): a power loss in between must not
    // start the new image without its marker. Both are keyed by the new image
    // id, so they are harmless should the activation below fail.
    //
    // The H5 marker (10.2): this image has to prove N2K reception before
    // OtaGuard marks it valid. Mode 0 expects the sender's INFO polls; 1 and
    // 2 (no restart now, or a sender that will not poll) any N2K frame.
    UpdNvs::H5 h5;
    memcpy(h5.imageId, info.imageId, 8);
    h5.mode = info.mode == APPLY_NOW ? UpdNvs::H5_PROTOCOL : UpdNvs::H5_ANY_FRAME;
    if (!UpdNvs::storeH5(h5))
        Serial.println("[ota] H5 marker could not be stored - the new image checks H1-H4 only");

    // 5.3.6: the web UI approved in the same dialog, usable once the new
    // firmware has been marked valid.
    if (info.companionSize) {
        UpdNvs::PreWeb pw;
        memcpy(pw.sha, info.companionSha, 32);
        pw.size = info.companionSize;
        memcpy(pw.imageId, info.imageId, 8);
        memcpy(pw.version, info.version, 16);
        if (!UpdNvs::storePreWeb(pw))
            Serial.println("[ota] companion approval could not be stored");
    } else {
        UpdNvs::clearPreWeb();
    }

    // One monolithic step (9.1): the IDF validates the image once more.
    const esp_err_t e = esp_ota_set_boot_partition(_part);
    if (e != ESP_OK) {
        Serial.printf("[ota] esp_ota_set_boot_partition failed: %s\n", esp_err_to_name(e));
        return APPLY_ST_ERROR;
    }
    UpdateGate::setRestartPending(true);
    noteActivated();
    Serial.printf("[ota] boot slot set to '%s' (mode %u)\n", _part->label, (unsigned)info.mode);
    _ended = false;
    _written = 0;                     // activated: abort() must not touch it any more
    release();
    return APPLY_ST_DONE;
}

void OtaWriter::abort() {
    if (_open) {
        esp_ota_abort(_h);
        _open = false;
    }
    // Something was written into the slot but it was never activated -
    // refused, abandoned, or complete but not applied. esp_ota_abort() erases
    // nothing, and the slot's otadata entry may still look valid from the
    // image that was there before: a crash-loop rollback (10.6) could start
    // this one as VALID, unchecked, maybe for another board. Erasing the
    // first sector makes sure nothing ever boots it. Nothing written means
    // the previous image is still intact there - and stays.
    if (_part && _written > 0) {
        const esp_err_t e = esp_partition_erase_range(_part, 0, 4096);
        if (e == ESP_OK) UpdNvs::clearSlotDirty();
        else Serial.printf("[ota] erasing the abandoned slot failed: %s\n", esp_err_to_name(e));
        _written = 0;
    }
    _ended = false;
    release();
}

// ---- WiFi upload ----------------------------------------------------------------------

// Keeps one match index per board name across calls; a name counts as seen
// once all its characters arrived in a row. The names never repeat their
// first character, so restarting a broken match at 0 (or 1, when the
// breaking byte starts a new one) is an exact search.
void OtaWriter::scanBoardNames(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char c = (char)p[i];
        for (int k = 0; k < 3; k++) {
            if (_nameSeen[k]) continue;
            const char *s = BOARD_NAMES[k];
            if (c == s[_nameMatch[k]]) {
                if (s[++_nameMatch[k]] == 0) _nameSeen[k] = true;
            } else {
                _nameMatch[k] = (c == s[0]) ? 1 : 0;
            }
        }
    }
}

uint8_t OtaWriter::identityCheck() {
    _descChecked = true;
    uint8_t *b = UpdRegions::bounce();
    uint8_t head[APP_DESC_OFFSET + 4];
    if (readBack(0, b, sizeof(head))) return RSN_FLASH_ERROR;
    memcpy(head, b, sizeof(head));
    if (readBack(FWDESC_OFFSET, b, FWDESC_SIZE)) return RSN_FLASH_ERROR;
    if (!isDescriptorMagic(b)) {
        // 8.6: images from before the descriptor (1.2.0 and older) are
        // refused over NMEA 2000 but stay installable over WiFi, or nobody
        // could ever go back to them. Their board is checked by name instead,
        // once the whole stream has been seen (finishAndActivate()).
        if (head[0] != IMAGE_MAGIC || getU32(head + APP_DESC_OFFSET) != APP_DESC_MAGIC)
            return RSN_IMAGE_INVALID;
        _legacy = true;
        Serial.println("[ota] image without descriptor (legacy) - board checked by name");
        return RSN_OK;
    }
    DescExpect e;
    e.boardId   = NP_BOARD_ID;
    e.panelW    = LCD_WIDTH;
    e.panelH    = LCD_HEIGHT;
    e.version16 = nullptr;            // no OFFER over WiFi: any version
    return checkImageIdentity(head, sizeof(head), b, FWDESC_SIZE, e);
}

uint8_t OtaWriter::writeAny(const uint8_t *src, size_t n) {
    if (!_open) return RSN_INTERNAL_ERROR;
    uint8_t *b = UpdRegions::bounce();
    while (n) {
        // Pieces end on 256-byte boundaries of the image offset, like the
        // receiver's, so each write enters at most one new sector.
        size_t piece = BOUNCE_BYTES - (_written % BOUNCE_BYTES);
        if (piece > n) piece = n;
        memcpy(b, src, piece);
        scanBoardNames(b, piece);
        uint8_t r = write(_written, b, piece);
        if (!r && !_descChecked && _written >= FWDESC_CHECK_BYTES) r = identityCheck();
        if (r) {
            Serial.printf("[ota] upload refused at %u bytes (reason 0x%02X)\n",
                          (unsigned)_written, r);
            abort();
            return r;
        }
        src += piece;
        n -= piece;
    }
    return RSN_OK;
}

uint8_t OtaWriter::finishAndActivate() {
    if (!_descChecked) { abort(); return RSN_SIZE_MISMATCH; }   // shorter than 0x160
    uint8_t r = finish();
    if (!r && _legacy) {
        const int own = NP_BOARD_ID - 1;
        bool ok = own >= 0 && own < 3 && _nameSeen[own];
        for (int k = 0; k < 3; k++) if (k != own && _nameSeen[k]) ok = false;
        if (!ok) {
            Serial.println("[ota] legacy image for another board - refused");
            r = RSN_WRONG_BOARD;
        }
    }
    // A WiFi install proves itself through H1-H4 only, and approves no web UI.
    // Cleared before the boot partition is set, so no marker of an earlier
    // N2K install can apply to this image (5.6 order, as in apply()).
    if (!r) {
        UpdNvs::clearH5();
        UpdNvs::clearPreWeb();
    }
    if (!r && esp_ota_set_boot_partition(_part) != ESP_OK) r = RSN_APPLY_FAILED;
    if (r) { abort(); return r; }
    UpdateGate::setRestartPending(true);
    noteActivated();
    Serial.printf("[ota] %u bytes written, boot slot set to '%s'\n",
                  (unsigned)_written, _part->label);
    _ended = false;
    _written = 0;                     // activated: abort() must not touch it any more
    release();
    return RSN_OK;
}
#endif  // SIMULATOR
