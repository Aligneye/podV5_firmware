#pragma once

#include <Arduino.h>
#include <RTTStream.h>

extern RTTStream rtt;

struct AlignRttSilencer {
    template <typename... Args>
    void print(Args...) {}

    template <typename... Args>
    void println(Args...) {}

    template <typename... Args>
    void printf(Args...) {}
};

// Dedicated RTT mirror for BLE traffic.
// Keep non-BLE RTT output disabled so the RTT monitor shows only app packets.
void rttDebuggerLoop();
void rttDebuggerPrintBlePacket(const char* direction, const char* payload);

static inline void logPacket(const char* channel, const char* payload) {
    (void)channel;
    (void)payload;
}

static inline void logEvent(const char* channel, const char* event) {
    (void)channel;
    (void)event;
}
