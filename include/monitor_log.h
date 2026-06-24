#pragma once

#include <Arduino.h>
#include <RTTStream.h>

extern RTTStream rtt;

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

