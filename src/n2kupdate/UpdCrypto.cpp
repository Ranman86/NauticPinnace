// ============================================================================
// UpdCrypto.cpp - see UpdCrypto.h.
// ============================================================================
#ifndef SIMULATOR
#include "UpdCrypto.h"
#include <esp_rom_crc.h>
#include <string.h>
#include <esp_ota_ops.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include <esp_app_desc.h>
#endif

MbedSha256::MbedSha256() { mbedtls_sha256_init(&_ctx); }
MbedSha256::~MbedSha256() { mbedtls_sha256_free(&_ctx); }

void MbedSha256::init() {
    // A fresh context per hash: free + init also drops any hardware engine
    // lock a previous, unfinished hash may still hold.
    mbedtls_sha256_free(&_ctx);
    mbedtls_sha256_init(&_ctx);
#if ESP_IDF_VERSION_MAJOR >= 5
    mbedtls_sha256_starts(&_ctx, 0);
#else
    mbedtls_sha256_starts_ret(&_ctx, 0);
#endif
    _live = true;
}

void MbedSha256::update(const uint8_t *data, size_t n) {
    if (!_live || !n) return;
#if ESP_IDF_VERSION_MAJOR >= 5
    mbedtls_sha256_update(&_ctx, data, n);
#else
    mbedtls_sha256_update_ret(&_ctx, data, n);
#endif
}

void MbedSha256::finish(uint8_t out[32]) {
    if (!_live) { memset(out, 0, 32); return; }
#if ESP_IDF_VERSION_MAJOR >= 5
    mbedtls_sha256_finish(&_ctx, out);
#else
    mbedtls_sha256_finish_ret(&_ctx, out);
#endif
    _live = false;
}

uint32_t updCrc32(uint32_t prev, const uint8_t *p, size_t n) {
    // The ROM routine has the zlib contract (start with 0, chain the result);
    // boot self-test 2 proves it against the check value 0xCBF43926.
    return esp_rom_crc32_le(prev, p, (uint32_t)n);
}

const esp_app_desc_t *updAppDesc() {
#if ESP_IDF_VERSION_MAJOR >= 5
    return esp_app_get_description();
#else
    return esp_ota_get_app_description();
#endif
}
#endif  // SIMULATOR
