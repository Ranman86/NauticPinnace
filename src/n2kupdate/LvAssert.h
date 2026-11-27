#pragma once
/* ============================================================================
 * LvAssert.h - what LV_ASSERT_HANDLER (lv_conf.h) needs, includable from C:
 * LVGL is C, and lv_conf.h names exactly one header for its assert handler.
 *
 * An LVGL assert restarts the device through esp_restart(), whose reset
 * reason (SW) looks like any deliberate restart. The crash-loop guard
 * (docs/N2K-UPDATE.md 10.6) has to count it as a crash, so the handler marks
 * it first in OtaGuard's RTC record.
 * ========================================================================== */
#include <stdio.h>
#include <esp_system.h>

#ifdef __cplusplus
extern "C" {
#endif
void otaGuardNoteAssert(void);
#ifdef __cplusplus
}
#endif
