#pragma once
// ============================================================================
// UpdCrypto - the small platform pieces the update receiver needs, behind one
// header so the IDF 4.4 / 5.5 split lives in exactly one place:
//
//   MbedSha256    n2kupd::ISha256 over mbedtls (hardware SHA, 8.4). IDF 4.4
//                 ships mbedtls 2.x (..._ret functions), IDF 5.5 mbedtls 3.x.
//                 The context is a member, so an instance in .bss lives in
//                 internal RAM - the hardware engine DMAs from its buffer.
//   updCrc32()    esp_rom_crc32_le, the n2kupd::Crc32Fn of the receiver (8.5).
//   updAppDesc()  the running image's esp_app_desc_t: IDF 4.4 has only
//                 esp_ota_get_app_description(), 5.5 esp_app_get_description().
//
// THE BOUNCE RULE (9.1). Every caller hands MbedSha256::update() the internal
// 256-byte bounce buffer (or another internal buffer), never PSRAM: on IDF 4.4
// the hardware SHA would DMA straight from PSRAM, which the 4-inch's OPI PSRAM
// coherency bug does not survive.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "N2kUpdateCore.h"

#ifndef SIMULATOR
#include <esp_idf_version.h>
#include <esp_ota_ops.h>        // esp_app_desc_t on both IDF versions
#include <mbedtls/sha256.h>

class MbedSha256 : public n2kupd::ISha256 {
public:
    MbedSha256();
    ~MbedSha256() override;
    void init() override;
    void update(const uint8_t *data, size_t n) override;
    void finish(uint8_t out[32]) override;
private:
    mbedtls_sha256_context _ctx;
    bool _live = false;
};

uint32_t updCrc32(uint32_t prev, const uint8_t *p, size_t n);
const esp_app_desc_t *updAppDesc();
#endif
