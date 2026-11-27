// ============================================================================
// N2kTx.cpp - see N2kTx.h.
// ============================================================================
#include "N2kTx.h"
#include <Arduino.h>
#include <NMEA2000.h>
#include <string.h>

extern tNMEA2000 &NMEA2000;

namespace N2kTx {
namespace {

struct Entry {
    uint32_t pgn;
    uint8_t  prio, dst, len;
    uint8_t  data[MAX_DATA];
};

// A spinlock, not a mutex: the critical sections are a 24-byte copy, and the
// posting side (the display task) must never wait on the bus task.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Entry    s_ring[SLOTS];
uint8_t  s_head = 0, s_count = 0;
uint32_t s_dropped = 0;

}  // namespace

bool post(const tN2kMsg &m) {
    if (m.DataLen < 0 || (size_t)m.DataLen > MAX_DATA) {
        portENTER_CRITICAL(&s_mux);
        s_dropped++;
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_count < SLOTS) {
        Entry &e = s_ring[(s_head + s_count) % SLOTS];
        e.pgn  = m.PGN;
        e.prio = m.Priority;
        e.dst  = m.Destination;
        e.len  = (uint8_t)m.DataLen;
        memcpy(e.data, m.Data, e.len);
        s_count++;
        ok = true;
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void drain() {
    for (;;) {
        Entry e;
        portENTER_CRITICAL(&s_mux);
        if (!s_count) { portEXIT_CRITICAL(&s_mux); return; }
        e = s_ring[s_head];
        s_head = (uint8_t)((s_head + 1) % SLOTS);
        s_count--;
        portEXIT_CRITICAL(&s_mux);

        tN2kMsg m;
        // Init() sets priority, PGN, destination and length and clears the
        // multi-packet flag; SendMsg() puts in our own source address.
        m.Init(e.prio, e.pgn, 0, e.dst);
        for (uint8_t i = 0; i < e.len; i++) m.AddByte(e.data[i]);
        NMEA2000.SendMsg(m);
    }
}

uint32_t dropped() {
    portENTER_CRITICAL(&s_mux);
    const uint32_t d = s_dropped;
    portEXIT_CRITICAL(&s_mux);
    return d;
}

}  // namespace N2kTx
