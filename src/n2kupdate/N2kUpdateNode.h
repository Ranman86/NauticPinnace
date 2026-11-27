#pragma once
// ============================================================================
// N2kUpdateNode - the device glue of "updates over NMEA 2000"
// (docs/N2K-UPDATE.md 9.1, 11.1, 11.2).
//
// Wires the platform-free receiver (N2kUpdateCore) to this firmware:
//
//   clock      millis()
//   link       NMEA2000.SendMsg() of PGN 126720 (fast-packet framed by the
//              library), GetN2kSource(), the driver's TxIdle()
//   host       the configuration, UpdateGate, OtaGuard, UpdRegions, LittleFS
//   ui         UpdUi - posted to the display task, never LVGL directly
//   targets    OtaWriter (firmware), WebUiWriter (web UI)
//   crypto     mbedtls SHA-256, the ROM CRC-32 and the ROM inflater
//
// EVERYTHING runs in the N2K task (core 0): onMessage() from the PGN 126720
// handler, tick() from the N2K loop in both branches (bus and demo), the
// user's Install / Decline / Cancel drained from the mailbox in tick().
//
// The receiver object itself (about 500 bytes) lives in PSRAM, in the block
// UpdRegions reserves at boot; internal RAM holds only the SHA context, the
// bounce buffer and the mailbox.
// ============================================================================
#include <stdint.h>

class tN2kMsg;

// Pause between two units of flash work (9.5). 0 by default; hardware test
// H20 decides whether the 7B/5B panel needs one.
#ifndef UPD_FLASH_GAP_MS
#define UPD_FLASH_GAP_MS 0
#endif

namespace N2kUpdateNode {

// setup(), right after UpdRegions::reserve(): constructs the receiver in its
// PSRAM block, runs the boot self-tests 1-4 of 8.1 (logged as
// "[n2kupd] selftest ok" / "selftest FAIL <n>"), installs the UI hooks.
void bootPrepare();

// N2kHandler::begin(), in the N2K task. Listen-only (canSend false): the
// display cannot answer and stays invisible to the protocol (5.1 rule 5);
// only the reception proof for H5 is kept.
void begin(bool canSend);

// N2kHandler::handleMsg(): a PGN 126720 message with the header FE 9F.
void onMessage(const tN2kMsg &msg);

// Every N2K loop pass, in the bus AND the demo branch.
void tick();

// ---- status (other tasks may read these) ------------------------------------------
uint8_t  sessionState();        // table 4.7
bool     sessionActive();       // CONFIRMING .. APPLYING
bool     selfTestsPassed();
uint8_t  capabilities();        // INFO byte 8
uint32_t n2kStackHighWater();   // bytes, for the [HB] line during a session

}  // namespace N2kUpdateNode
