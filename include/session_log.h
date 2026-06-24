#pragma once

#include <Arduino.h>

static const int MAX_SESSIONS = 120;
static const uint8_t MAX_POSTURE_EVENTS = 64u;
static const uint8_t MAX_THERAPY_PATTERNS = 30u;

#define SESSION_FILE "/sessions.dat"
#define SESSION_EVENTS_FILE "/sess_ev.dat"
static const uint32_t SESSION_EVENT_MAGIC = 0x53455631u; // "SEV1"

enum SessionType : uint8_t {
    SESSION_TYPE_POSTURE = 1,
    SESSION_TYPE_THERAPY = 2
};

struct StoredSession {
    uint8_t  type;
    uint32_t start_ts;
    bool     ts_synced;
    uint16_t duration_sec;
    uint16_t wrong_count;
    uint16_t wrong_dur_sec;
    uint8_t  therapy_pattern;
    bool     sent;
};

struct __attribute__((packed)) EventRecordHeader {
    uint32_t magic;
    uint8_t  type;
    uint8_t  count;
    uint32_t session_ts;
    uint16_t payloadLen;
};

struct PostureEventReadResult {
    uint8_t pairCount;
    uint16_t slouchOffsets[MAX_POSTURE_EVENTS];
    uint16_t correctionOffsets[MAX_POSTURE_EVENTS];
};

struct TherapyEventReadResult {
    uint8_t count;
    uint8_t patterns[MAX_THERAPY_PATTERNS];
};

void session_log_init();
void session_log_append(const StoredSession& s);
int  session_log_count_unsent();
bool session_log_get_unsent(int index, StoredSession& out, int& fileIndex);
void session_log_mark_sent(int fileIndex);
void session_log_purge_sent();

void session_log_write_training_events(uint32_t sessionTs,
                                       const uint16_t* slouchBuf,
                                       const uint16_t* correctionBuf,
                                       uint16_t slouchCount,
                                       uint16_t correctionCount);

void session_log_write_therapy_events(uint32_t sessionTs,
                                      const uint8_t* patternBuf,
                                      uint8_t patternCount);

bool session_log_read_posture_events(uint32_t sessionTs,
                                     PostureEventReadResult& out);
bool session_log_read_therapy_events(uint32_t sessionTs,
                                     TherapyEventReadResult& out);
