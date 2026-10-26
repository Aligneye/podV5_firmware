#pragma once

#include <Arduino.h>

// ── Session log v2 ───────────────────────────────────────────────────────
// Every completed session is one self-contained record in SESSION_LOG_FILE:
//
//   header  : 'SREC' · ver · type · flags · rsv · session_id · start_ts · body_len
//   body    : events (training: start/dur pairs, therapy: pattern/start/dur)
//             + totals (training: total/slouches/bad/good, therapy: total/patterns)
//   footer  : 'SEND' · session_id · crc32(header..body)
//
// A sync acknowledgement appends a 'SACK' · session_id · crc32 marker.
// A record only counts if both magics, both ids and the CRC agree, so a
// torn write is detected and dropped instead of corrupting later records.
// All multi-byte fields are little-endian. See docs/session_sync_v2.md.

static const int      MAX_SESSIONS         = 120;
// Slouch periods kept per training record (more sets SREC_FLAG_EVENTS_TRUNCATED;
// totals still cover the whole session).
static const uint16_t MAX_STORED_EVENTS    = 64u;
static const uint8_t  MAX_THERAPY_PATTERNS = 30u;

#define SESSION_LOG_FILE "/sess_v2.log"

static const uint32_t SREC_MAGIC   = 0x43455253u;  // "SREC"
static const uint32_t SEND_MAGIC   = 0x444E4553u;  // "SEND"
static const uint32_t SACK_MAGIC   = 0x4B434153u;  // "SACK"
static const uint8_t  SREC_VERSION = 1u;

enum SessionType : uint8_t {
    SESSION_TYPE_POSTURE = 1,
    SESSION_TYPE_THERAPY = 2
};

enum : uint8_t {
    SREC_FLAG_TIME_SYNCED      = 0x01,  // start_ts came from a fresh RTC sync
    SREC_FLAG_EVENTS_TRUNCATED = 0x02   // more events happened than were stored
};

// One stored event. Training: a slouch period (pattern unused).
// Therapy: one pattern run. Offsets are seconds from session start.
struct SessionEvent {
    uint8_t  pattern;
    uint16_t start_off;
    uint16_t dur;
};

// Decoded form of one session record (what gets written and read back).
struct SessionRecord {
    uint8_t  type;
    uint8_t  flags;
    uint32_t session_id;
    uint32_t start_ts;
    uint16_t total_sec;
    // Training totals
    uint16_t slouch_count;
    uint16_t bad_sec;
    uint16_t good_sec;
    // Therapy totals
    uint8_t  pattern_count;
    uint16_t event_count;
    SessionEvent events[MAX_STORED_EVENTS];
};

// RAM index entry for a stored record (the record body stays on flash).
struct SessionIndexEntry {
    uint32_t session_id;
    uint32_t start_ts;
    uint32_t file_off;
    uint16_t len;
    uint8_t  type;
    bool     sent;
};

void session_log_init();
// Flushes queued appends from the main loop so session-end writes never
// block a button press. Call every loop iteration.
void session_log_loop();
// Queues a completed session for storage. Returns false only if it could
// not be queued at all.
bool session_log_append(const SessionRecord& rec);
int  session_log_count_unsent();
bool session_log_get_unsent(int index, SessionIndexEntry& out);
// Reads and CRC-checks the stored record with this id.
bool session_log_read(uint32_t sessionId, SessionRecord& out);
void session_log_mark_sent(uint32_t sessionId);
// Drops sent records and compacts the log file.
void session_log_purge_sent();
// Highest session id currently stored (0 if none). Used to keep new ids unique.
uint32_t session_log_max_id();

// Fills a therapy record's pattern events from the played sequence. Each
// pattern ran patternSec seconds; the last one runs until the session ended.
// r.total_sec must already be set.
void session_record_fill_therapy(SessionRecord& r, const uint8_t* patterns,
                                 uint8_t count, uint16_t patternSec);
