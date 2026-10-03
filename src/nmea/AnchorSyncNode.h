#pragma once
// ============================================================================
// AnchorSyncNode - this device's end of the anchor-state sync (see AnchorSync.h
// for the wire format and the arbitration rules).
//
// Split from AnchorSync.cpp on purpose: the codec there depends on nothing but
// stdint and can therefore be compiled and checked on a host. Everything that
// needs the config, the data model or the bus lives here.
//
// THREADING. Messages arrive on the N2K task (core 0); the config and the alarm
// belong to the display loop (core 1). A decoded state that wins arbitration is
// therefore parked in a pending slot and applied by applyPending() from
// DisplayManager::update() - the same shape as the pending screen-config patch
// next to it. Nothing here writes LittleFS from the bus task.
// ============================================================================
// Deliberately NOT <NMEA2000.h>: the anchor screen, the web handler and the
// display loop all include this header, and on the PC simulator there is no
// NMEA 2000 library at all. A reference to an incomplete type is enough for
// the one declaration that needs it.
class tN2kMsg;
#include "AnchorSync.h"

// Current UTC in Unix seconds, or 0 when this device has no idea what time
// it is. Zero is a legitimate answer, not an error: a device without a
// clock still syncs, because the revision stamp falls back to own + 1.
uint32_t anchorSyncNowUtcOrZero();

namespace AnchorSyncNode {

// Runs the codec self test and reports whether sending is possible at all.
// Called from N2kHandler::begin(), on the bus task, after NMEA2000.Open().
void begin(bool canSend);

// Bus task: sends the heartbeat and any pending immediate broadcast.
// begin(), loop() and onMessage() are no-ops in the simulator build.
void loop();

// Bus task: an ANCHOR_SYNC_PGN frame arrived.
void onMessage(const tN2kMsg &msg);

// Call after ANY local change to the anchor state - set, lift, radius, arming,
// from the screen or from the web handler. Stamps a new revision and asks for
// an immediate broadcast. Must NOT be called when adopting a foreign state:
// adopting is not a change, and stamping it would bounce the state back.
void noteLocalChange();

// Display task: applies a state adopted from the bus. Returns true if the
// anchor state changed, in which case the caller re-evaluates the alarm at
// once rather than waiting for the next cycle.
bool applyPending();

}  // namespace AnchorSyncNode
