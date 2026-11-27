#pragma once
// ============================================================================
// RomInflater - n2kupd::IInflater over the ROM copy of miniz's
// tinfl_decompress() (docs/N2K-UPDATE.md 9.3).
//
// The ESP32-S3 ROM has tinfl_decompress at 0x40000828, the same address under
// IDF 4.4 (4-inch) and 5.5 (7B/5B). Its decompressor state is the IDF header's
// tinfl_decompressor, about 11 KB - far too much for internal RAM, so it lives
// in the PSRAM work region (UpdRegions), next to the 32 KB dictionary. The
// CPU reads and writes PSRAM here; no DMA and no flash operation is involved.
//
// Boot self-test 4 (8.1) decodes the Appendix A vector through exactly this
// object, which proves that the header's struct layout matches the ROM's.
// ============================================================================
#include "N2kUpdateCore.h"

#ifndef SIMULATOR
class RomInflater : public n2kupd::IInflater {
public:
    // Bytes the decompressor state needs; UpdRegions reserves room for it.
    static size_t stateBytes();
    bool init(void *state) override;
    int decompress(const uint8_t *in, size_t *inBytes, uint8_t *dict,
                   size_t dictPos, size_t *outBytes, bool hasMoreInput) override;
private:
    void *_state = nullptr;
};
#endif
