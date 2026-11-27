#pragma once
// ============================================================
// BshTide – German tide forecast (BSH Wasserstandsvorhersage).
//
// Device-only (NOT compiled into the PC simulator). Starts one background
// FreeRTOS task that, when WiFi is connected:
//   1. syncs the system clock via SNTP (Europe/Berlin TZ) so the real current
//      time is known even without a GPS fix → sets data.timeIsReal. The time
//      server is the one the router names in its DHCP lease, if any, else
//      pool.ntp.org (also when the router's one stays silent for 10 min),
//   2. fetches the nearest German tide gauge's HW/NW predictions from the BSH
//      OGC API (gdi.bsh.de, CC BY 4.0) every 3 h and stores them in the global
//      DataModel. The request carries the position rounded to 0.1 degree.
// The ClockScreen shows these official values when fresh and otherwise falls
// back to its own astronomical estimate.
// ============================================================
void bshTideBegin();

// Lets lwIP take the time server from the DHCP lease (option 42). lwIP looks
// at it only when a lease comes in, so call this after the radio is up and
// BEFORE the first join; bshTideBegin() falls back to the pool by itself.
void bshSntpPreferDhcp();

// Keeps data.localOffsetMin on the Europe/Berlin rule between SNTP syncs, so a
// DST switch shows on time. Call from loop(); rate-limited to once a second and a
// no-op until SNTP has set the offset, and while a bus offset (PGN 129033)
// holds it - the next sync and clearValues() hand it back to the rule.
void bshLocalOffsetTick();
