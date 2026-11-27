#pragma once
// ============================================================================
// ApPipeline - the one place the pilot computer's mode enters the DataModel.
//
// Thin glue around ApDecode (the rules are there): it keeps the table of
// NAMEs learnt from the address claims, decodes the three proprietary PGNs of
// manufacturer code 1857 with it, feeds the result to data.apTrack (priority,
// debounce, staleness) and copies the outcome into data.apMode & co.
//
// READ ONLY - nothing here transmits; see ApDecode.h.
//
// The device's N2K task calls it from N2kHandler::handleMsg; the PC simulator
// calls the very same functions with synthetic example frames (--ap-frames;
// built from the protocol description, not a bus recording), and demo mode
// goes in through demo(). The clock (tick) runs from the display tick, in
// every mode, so a pilot computer that falls silent turns into "--" even
// while no frame arrives at all.
//
// REMEMBERED PILOT COMPUTER. The NAME table is learnt from address claims,
// and a display that restarts on its own (settings saved, an update, a power
// blip) hears none - the pilot claimed long ago. So the pilot computer whose
// frames decide the mode is remembered across a restart (NAME + address, in
// NVS on the device, in memory in the simulator) and entered into the table
// at boot. A claim on the bus overrides it as usual: the remembered NAME
// claiming another address moves it, another NAME claiming that address or a
// "cannot claim" forgets it. Nothing is sent for this either.
// ============================================================================
#include <stdint.h>
#include "ApDecode.h"

namespace ApPipeline {

// ---- Bus task only (or the simulator's frame player) -----------------------
// The NAME table and the remembered pilot are read and written by that one
// task alone, so these take no lock.
//
// At start-up, before the first frame: enters the remembered pilot computer
// (if any) into the NAME table.
void begin();
// PGN 60928, as received (8 data bytes).
void addressClaim(uint8_t sa, const uint8_t *d, uint8_t len);
// Does the NAME known for this address say "pilot computer"? Asked BEFORE a
// frame goes to the source picker's bookkeeping, so the picker lists pilot
// computers only.
bool isPilotComputer(uint8_t sa);
// Writes a changed remembered pilot to NVS. From the bus task's loop, NOT
// under data.lock(): a flash write takes milliseconds and must not hold up
// the display. Does nothing most of the time (a write happens only when the
// deciding pilot computer or its address changes).
void persistDue();
// Forget every NAME, as a restart does (the simulator; the remembered pilot
// survives - begin() enters it again).
void resetNames();

// ---- With data.lock() held (the mutex is not recursive) --------------------
// One frame of PGN 65305, 65340 or 65341. Returns what the decoder made of it,
// ignored frames included.
ApDecode::Facts frame(uint32_t pgn, uint8_t sa, const uint8_t *d, uint8_t len,
                      uint32_t nowMs);
// Demo mode: the scenario's pilot state, refreshed on every demo tick.
void demo(ApDecode::Mode mode, ApDecode::Status status,
          ApDecode::SetpointKind kind, float setpointDeg, uint32_t nowMs);
// The clock: lets a silent pilot computer go stale. Display tick.
void tick(uint32_t nowMs);

} // namespace ApPipeline
