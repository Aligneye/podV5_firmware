#pragma once

#include <Arduino.h>
#include <RTTStream.h>

extern RTTStream rtt;

// Dedicated RTT mirror for BLE traffic and local debugging.
// Keep BLE packet formatting in bluetooth.cpp; this helper only mirrors it to RTT.
// Current stats are emitted from a single place for easier local debugging.
void rttDebuggerLoop();
void rttDebuggerPrintBlePacket(const char* direction, const char* payload);

static inline void logPacket(const char* channel, const char* payload) {
    if (!channel || !payload) return;
    rtt.print("[");
    rtt.print(channel);
    rtt.print("] ");
    rtt.println(payload);
}

static inline void logEvent(const char* channel, const char* event) {
    if (!channel || !event) return;
    rtt.print("[");
    rtt.print(channel);
    rtt.print("] ");
    rtt.println(event);
}
