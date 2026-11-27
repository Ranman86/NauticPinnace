// ============================================================================
// RomInflater.cpp - see RomInflater.h.
// ============================================================================
#ifndef SIMULATOR
#include "RomInflater.h"
#include "UpdRegions.h"
#include <esp_idf_version.h>
// The IDF's own declaration of the ROM routine, so the struct layout is the
// one the ROM was built with: IDF 4.4 keeps it per chip, 5.5 in esp_rom.
#if ESP_IDF_VERSION_MAJOR >= 5
#include <miniz.h>
#else
#include <esp32s3/rom/miniz.h>
#endif

static_assert(TINFL_LZ_DICT_SIZE == n2kupd::DICT_BYTES,
              "the receiver's dictionary must be tinfl's LZ dictionary");
static_assert(sizeof(tinfl_decompressor) <= UpdRegions::INFLATER_STATE_BYTES,
              "tinfl_decompressor does not fit the room the work region reserves for it");

size_t RomInflater::stateBytes() { return sizeof(tinfl_decompressor); }

bool RomInflater::init(void *state) {
    if (!state) return false;
    _state = state;
    tinfl_init((tinfl_decompressor *)state);
    return true;
}

int RomInflater::decompress(const uint8_t *in, size_t *inBytes, uint8_t *dict,
                            size_t dictPos, size_t *outBytes, bool hasMoreInput) {
    if (!_state) return -3;   // TINFL_STATUS_BAD_PARAM
    // Raw deflate (no zlib header) into the wrapping 32 KB dictionary: no
    // PARSE_ZLIB_HEADER, no NON_WRAPPING_OUTPUT_BUF (9.3). The output space is
    // always [dictPos, 32768), the only size tinfl accepts for a wrapping
    // buffer.
    const mz_uint32 flags = hasMoreInput ? TINFL_FLAG_HAS_MORE_INPUT : 0;
    return (int)tinfl_decompress((tinfl_decompressor *)_state, in, inBytes,
                                 dict, dict + dictPos, outBytes, flags);
}
#endif  // SIMULATOR
