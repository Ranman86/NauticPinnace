#pragma once
// ============================================================================
// N2kTx - the cross-task send mailbox (docs/N2K-UPDATE.md 2.3 rule 7, 11.2).
//
// The NMEA2000 library is not thread-safe: its send ring and its per-PGN
// fast-packet sequence counters are plain members. The media screen used to
// call NMEA2000.SendMsg() straight from the LVGL task, and its PGN 126720
// commands could then interleave with the update receiver's own 126720
// messages from the same source address - which breaks the fast-packet rule
// that no frame of another 126720 message may come between the frames of
// one. Since this module exists, NO task other than the N2K task calls
// SendMsg(): everyone else posts here, and the N2K task drains the ring on
// every loop pass.
//
// Small on purpose: 8 entries of at most 16 data bytes (media commands are 6
// at most). A full ring drops the newest message and counts it - a lost
// volume step is harmless, a blocked UI task is not.
// ============================================================================
#include <stdint.h>
#include <stddef.h>

class tN2kMsg;

namespace N2kTx {

static constexpr size_t SLOTS    = 8;
static constexpr size_t MAX_DATA = 16;

// Any task. Copies PGN, priority, destination and up to MAX_DATA bytes;
// returns false (and counts) if the ring is full or the message too long.
bool post(const tN2kMsg &m);

// N2K task only: sends everything posted so far with NMEA2000.SendMsg().
void drain();

uint32_t dropped();   // posts lost to a full ring or an oversized message

}  // namespace N2kTx
