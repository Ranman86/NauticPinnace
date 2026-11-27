/* ============================================================================
 * FwDesc.c - the one definition of np_fw_desc (see FwDesc.h).
 *
 * Every value comes from the same macros the rest of the firmware uses, so the
 * descriptor cannot drift from the build: the board id and name from the one
 * board flag of the env (Version.h refuses a build without exactly one), the
 * panel size from BoardConfig, the version from Version.h.
 * ========================================================================== */
#ifndef SIMULATOR
#include "FwDesc.h"
#include "../Version.h"
#include "../BoardConfig.h"

const np_fw_desc_t np_fw_desc __attribute__((section(".rodata_custom_desc"), used)) = {
    .magic            = {'N', 'P', 'F', 'W', 'D', 'E', 'S', 'C'},
    .desc_version     = 1,
    .board_id         = NP_BOARD_ID,
    .desc_size        = sizeof(np_fw_desc_t),
    .panel_w          = LCD_WIDTH,
    .panel_h          = LCD_HEIGHT,
    .n2k_update_proto = 1,
    .flags            = NP_FWDESC_F_OTAGUARD,
    .reserved         = {0, 0},
    .fw_version       = FW_VERSION,
    .board_name       = FW_BOARD_NAME,
};
#endif
