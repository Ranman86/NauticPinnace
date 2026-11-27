#pragma once
// ============================================================================
// OtaWriter - the firmware writer (docs/N2K-UPDATE.md 9.2), shared by the
// update over NMEA 2000 and the WiFi upload (/api/ota).
//
//   begin    esp_ota_begin(next slot, OTA_WITH_SEQUENTIAL_WRITES): the IDF
//            erases 4 KB sectors on demand inside esp_ota_write, never the
//            whole slot up front (Arduino's Update.h erases 64 KB blocks and
//            holds a 4 KB DRAM buffer - neither is affordable here). It
//            refuses while the running image is PENDING_VERIFY.
//   write    strictly sequential, at most 256 bytes per call, always from the
//            internal bounce buffer (bounce rule, 9.1).
//   finish   esp_ota_end: checks the image structure and the appended hash.
//   readBack esp_partition_read into the bounce buffer.
//   apply    the H5 marker (10.2) and the companion approval (5.3.6) in NVS,
//            then esp_ota_set_boot_partition - in that order, so a power
//            loss in between cannot start the image without its marker.
//   abort    esp_ota_abort; tolerates every state, also after finish(). A
//            slot that was written but never activated gets its first
//            sector erased, so no fallback can ever boot it.
//
// NVS n2kupd/dirty marks the slot as being written from begin() until it is
// activated or erased again (the rollback report must not take what is in
// it for a rolled-back image).
//
// Exactly one owner at a time (UpdateGate::claimWriter): the N2K session or
// one WiFi upload. begin() claims, abort() and a finished apply release.
//
// The WiFi path adds writeAny(): it accepts data from anywhere (lwIP buffers,
// PSRAM), copies it through the bounce buffer, and runs the descriptor check
// of 8.2 from flash as soon as 0x160 bytes are written - the same check the
// N2K receiver runs, so a 4-inch image never reaches a 7B over WiFi either.
// ============================================================================
#include "N2kUpdateCore.h"
#include "UpdateGate.h"

#ifndef SIMULATOR
#include <esp_ota_ops.h>

class OtaWriter : public n2kupd::IUpdTarget {
public:
    explicit OtaWriter(UpdateGate::Owner owner) : _owner(owner) {}

    // ---- n2kupd::IUpdTarget ---------------------------------------------------------
    uint8_t begin(uint32_t imageSize) override;
    uint8_t write(uint32_t offset, const uint8_t *bounce, size_t n) override;
    uint8_t finish() override;
    uint8_t readBack(uint32_t offset, uint8_t *bounce, size_t n) override;
    n2kupd::ApplyStatus apply(const n2kupd::ApplyInfo &info, uint8_t *detail) override;
    void abort() override;

    // ---- WiFi upload ------------------------------------------------------------------
    // Any source buffer; the identity check runs inside. Returns a reason code
    // (4.8); after a failure the writer is aborted and released.
    uint8_t writeAny(const uint8_t *src, size_t n);
    // finish() plus, for a legacy image without descriptor, the board-name
    // check below, then esp_ota_set_boot_partition. Releases the writer.
    uint8_t finishAndActivate();
    uint32_t written() const { return _written; }
    bool legacyImage() const { return _legacy; }

    // millis() of the last activation in this boot (either path); false if
    // none. A restart long after it is met by no sender polling (H5).
    static bool activatedAt(uint32_t &ms);

private:
    uint8_t identityCheck();     // 8.2 from flash, WiFi path only
    void    scanBoardNames(const uint8_t *p, size_t n);
    void    release();

    UpdateGate::Owner      _owner;
    const esp_partition_t *_part = nullptr;
    esp_ota_handle_t       _h = 0;
    bool     _open = false;          // between esp_ota_begin and esp_ota_end/abort
    bool     _ended = false;         // esp_ota_end succeeded
    bool     _claimed = false;
    uint32_t _written = 0;
    bool     _descChecked = false;
    bool     _legacy = false;        // WiFi: an image without descriptor (8.6)
    // WiFi legacy images: which FW_BOARD_NAME strings the stream contained.
    uint8_t  _nameMatch[3] = { 0, 0, 0 };
    bool     _nameSeen[3]  = { false, false, false };
};
#endif
