#pragma once
// ============================================================
// BshTide – German tide forecast (BSH Wasserstandsvorhersage).
//
// Device-only (NOT compiled into the PC simulator). Starts one background
// FreeRTOS task that, when WiFi is connected:
//   1. syncs the system clock via SNTP (Europe/Berlin TZ) so the real current
//      time is known even without a GPS fix → sets data.timeIsReal,
//   2. fetches the nearest German tide gauge's HW/NW predictions from the BSH
//      OGC API (gdi.bsh.de, CC BY 4.0) and stores them in the global DataModel.
// The ClockScreen shows these official values when fresh and otherwise falls
// back to its own astronomical estimate.
// ============================================================
void bshTideBegin();
