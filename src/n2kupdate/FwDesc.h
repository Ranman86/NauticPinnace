#pragma once
/* ============================================================================
 * FwDesc - the firmware descriptor np_fw_desc_t (docs/N2K-UPDATE.md 8.1).
 *
 * 64 bytes at image file offset 0x120, directly behind esp_app_desc_t. That
 * structure cannot tell the boards apart (its project name is the same on 7B
 * and 5B, and "arduino-lib-builder" on the 4-inch), so every image carries
 * this one as well: board id, panel size and version, read by the sender
 * before it offers a file and by the display, from flash, before anything is
 * activated (8.2).
 *
 * C, not C++: a namespace-scope const in C++ has internal linkage and would be
 * dropped. The section .rodata_custom_desc is placed right after .rodata_desc
 * by the linker scripts of both cores; -Wl,-u,np_fw_desc in every firmware env
 * keeps it through --gc-sections. The layout is checked by the boot self-test
 * 1 (its distance from the app description must be exactly 0x100) and by
 * tools/release_checks.py on every released image.
 * ========================================================================== */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_FWDESC_F_OTAGUARD 0x01   /* the image confirms itself through OtaGuard (10.2) */

typedef struct __attribute__((packed)) {
    char     magic[8];          /* "NPFWDESC" */
    uint8_t  desc_version;      /* 1 */
    uint8_t  board_id;          /* 1 = 4-inch, 2 = 7B, 3 = 5B */
    uint16_t desc_size;         /* 64 */
    uint16_t panel_w, panel_h;  /* LCD_WIDTH / LCD_HEIGHT */
    uint8_t  n2k_update_proto;  /* highest protocol version of this receiver: 1 */
    uint8_t  flags;             /* NP_FWDESC_F_OTAGUARD */
    uint8_t  reserved[2];
    char     fw_version[16];    /* FW_VERSION, NUL-padded */
    char     board_name[28];    /* FW_BOARD_NAME, NUL-padded */
} np_fw_desc_t;

#ifdef __cplusplus
static_assert(sizeof(np_fw_desc_t) == 64, "np_fw_desc_t must be 64 bytes");
#else
_Static_assert(sizeof(np_fw_desc_t) == 64, "np_fw_desc_t must be 64 bytes");
#endif

#ifndef SIMULATOR
/* The simulator links no descriptor (4.1: simulator builds carry none). */
extern const np_fw_desc_t np_fw_desc;
#endif

#ifdef __cplusplus
}
#endif
