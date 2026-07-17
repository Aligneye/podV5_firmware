#include "session_log.h"
#include "rtt_debugger.h"
#include <string.h>
#include <RTTStream.h>

extern RTTStream rtt;
// Real RTT stream for flash-write timing measurements; the `rtt` macro below
// silences all other non-BLE output in this file.
static RTTStream& s_rttTiming = rtt;
static AlignRttSilencer s_nonBleRtt;
#define rtt s_nonBleRtt

#if __has_include(<InternalFileSystem.h>)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#define SESSION_LOG_HAS_FS 1
#else
#define SESSION_LOG_HAS_FS 0
#endif

namespace {

constexpr const char* kTempPath    = "/sessions.tmp";
constexpr const char* kEvTempPath  = "/sess_ev.tmp";

// /sessions.log is an append-only log. A session end appends one small
// record; a sync acknowledgement appends a tiny "sent" marker. The file is
// rewritten compactly only when sent sessions are purged after a sync or
// when it outgrows kLogCompactBytes — never on the session-end hot path.
// (The legacy /sessions.dat whole-array file is migrated once at boot.)
constexpr const char* kLogPath = "/sessions.log";
constexpr uint8_t  kRecSession     = 0xA5;
constexpr uint8_t  kRecSentMark    = 0x5A;
constexpr uint32_t kLogCompactBytes = 4096u;
constexpr uint32_t kFlushDelayMs    = 400u;

StoredSession g_sessions[MAX_SESSIONS];
int           g_count    = 0;
bool          g_ready    = false;

// Pending appends are buffered in RAM and flushed from session_log_loop(),
// so a session ending on a button press never blocks on flash.
uint8_t  g_pendingBuf[192];
size_t   g_pendingLen     = 0;
uint32_t g_pendingSinceMs = 0;

int findOldestSentSlot() {
    for (int i = 0; i < g_count; i++) {
        if (g_sessions[i].sent) return i;
    }
    return -1;
}

void removeSlot(int idx) {
    if (idx < 0 || idx >= g_count) return;
    for (int i = idx; i < g_count - 1; i++) {
        g_sessions[i] = g_sessions[i + 1];
    }
    g_count--;
}

void addSessionToRam(const StoredSession& s) {
    if (g_count >= MAX_SESSIONS) {
        int victim = findOldestSentSlot();
        // All unsent: sacrifice the oldest so the newest session is kept
        // (the old code dropped the incoming session instead).
        if (victim < 0) victim = 0;
        removeSlot(victim);
    }
    g_sessions[g_count++] = s;
}

#if SESSION_LOG_HAS_FS
// Rebuild RAM state by replaying the append log. Session records are added
// in order (evicting per policy if over cap); sent markers flip the flag on
// matching sessions. A torn/unknown record ends the replay — everything
// before it is intact because LittleFS commits appends atomically on close.
bool replayLog() {
    File f = InternalFS.open(kLogPath, FILE_O_READ);
    if (!f) return false;

    while (true) {
        uint8_t kind = 0;
        if (f.read(&kind, 1) != 1) break;
        if (kind == kRecSession) {
            StoredSession s{};
            if (f.read((uint8_t*)&s, sizeof(s)) != (int)sizeof(s)) break;
            if (s.type != SESSION_TYPE_POSTURE && s.type != SESSION_TYPE_THERAPY) break;
            addSessionToRam(s);
        } else if (kind == kRecSentMark) {
            uint32_t ts = 0;
            if (f.read((uint8_t*)&ts, sizeof(ts)) != (int)sizeof(ts)) break;
            for (int i = 0; i < g_count; i++) {
                if (g_sessions[i].start_ts == ts) g_sessions[i].sent = true;
            }
        } else {
            break;
        }
    }
    f.close();
    return true;
}

// Rewrite the log as a compact snapshot of RAM state (in-place overwrite;
// LittleFS keeps the old contents valid until close() commits). Runs only
// after sync purges or when the log outgrows kLogCompactBytes.
bool rewriteLogCompact() {
    const uint32_t t0 = millis();
    g_pendingLen = 0;  // snapshot supersedes any queued records

    File f = InternalFS.open(kLogPath, FILE_O_WRITE);
    if (!f) return false;

    f.seek(0);  // FILE_O_WRITE opens positioned at end of file
    size_t total = 0;
    bool ok = true;
    for (int i = 0; i < g_count && ok; i++) {
        ok = f.write(&kRecSession, 1) == 1 &&
             f.write((uint8_t*)&g_sessions[i], sizeof(StoredSession)) == sizeof(StoredSession);
        total += 1 + sizeof(StoredSession);
    }
    f.truncate(ok ? total : 0);
    f.close();

    s_rttTiming.print("[TIMING] session log compact: ");
    s_rttTiming.print(millis() - t0);
    s_rttTiming.println(" ms");
    return ok;
}

void flushPending() {
    if (g_pendingLen == 0) return;
    const uint32_t t0 = millis();

    File f = InternalFS.open(kLogPath, FILE_O_WRITE);  // positioned at end
    if (!f) return;  // keep pending; retried from session_log_loop()

    const size_t written = f.write(g_pendingBuf, g_pendingLen);
    const uint32_t logSize = f.size();
    f.close();

    if (written != g_pendingLen) {
        // Partial append would leave a torn tail; rebuild from RAM instead.
        rewriteLogCompact();
        return;
    }
    g_pendingLen = 0;

    s_rttTiming.print("[TIMING] session log append: ");
    s_rttTiming.print(millis() - t0);
    s_rttTiming.println(" ms");

    if (logSize > kLogCompactBytes) {
        rewriteLogCompact();
    }
}

void queueRecord(uint8_t kind, const void* payload, size_t len) {
    if (g_pendingLen + 1 + len > sizeof(g_pendingBuf)) {
        flushPending();  // synchronous fallback if the queue overflows
        if (g_pendingLen + 1 + len > sizeof(g_pendingBuf)) return;
    }
    if (g_pendingLen == 0) g_pendingSinceMs = millis();
    g_pendingBuf[g_pendingLen++] = kind;
    memcpy(g_pendingBuf + g_pendingLen, payload, len);
    g_pendingLen += len;
}

void loadFromDisk() {
    g_count = 0;

    if (replayLog()) return;

    // One-time migration from the legacy whole-array /sessions.dat.
    File file = InternalFS.open(SESSION_FILE, FILE_O_READ);
    if (!file) return;

    while (g_count < MAX_SESSIONS) {
        StoredSession tmp{};
        int rd = file.read((uint8_t*)&tmp, sizeof(tmp));
        if (rd != (int)sizeof(tmp)) break;
        if (tmp.type != SESSION_TYPE_POSTURE && tmp.type != SESSION_TYPE_THERAPY) {
            continue;
        }
        g_sessions[g_count++] = tmp;
    }
    file.close();

    if (rewriteLogCompact()) {
        InternalFS.remove(SESSION_FILE);
    }
    InternalFS.remove(kTempPath);  // stale temp from the old rewrite pattern
}

bool appendToEventFile(const uint8_t* data, size_t len) {
    File f = InternalFS.open(SESSION_EVENTS_FILE, FILE_O_WRITE);
    if (!f) return false;
    f.seek(f.size());
    size_t w = f.write(data, len);
    f.flush();
    f.close();
    return w == len;
}

bool findEventRecord(uint32_t ts, EventRecordHeader& outHdr,
                     uint32_t& outPayloadOffset) {
    File f = InternalFS.open(SESSION_EVENTS_FILE, FILE_O_READ);
    if (!f) return false;
    uint32_t fileSize = (uint32_t)f.size();
    uint32_t pos = 0;
    bool found = false;
    while (pos + sizeof(EventRecordHeader) <= fileSize) {
        EventRecordHeader hdr{};
        if (f.read((uint8_t*)&hdr, sizeof(hdr)) != (int)sizeof(hdr)) break;
        if (hdr.magic != SESSION_EVENT_MAGIC) break;
        uint32_t recEnd = pos + sizeof(hdr) + hdr.payloadLen;
        if (recEnd > fileSize) break;
        if (hdr.session_ts == ts) {
            outHdr = hdr;
            outPayloadOffset = pos + sizeof(hdr);
            found = true;
            break;
        }
        pos = recEnd;
        if (!f.seek(pos)) break;
    }
    f.close();
    return found;
}

void collectActiveTimestamps(uint32_t* tsBuf, int& count, int maxCount) {
    count = 0;
    for (int i = 0; i < g_count && count < maxCount; i++) {
        if (!g_sessions[i].sent && g_sessions[i].start_ts != 0) {
            tsBuf[count++] = g_sessions[i].start_ts;
        }
    }
}

bool isTimestampActive(uint32_t ts, const uint32_t* tsBuf, int tsCount) {
    for (int i = 0; i < tsCount; i++) {
        if (tsBuf[i] == ts) return true;
    }
    return false;
}

void purgeOrphanedEvents() {
    File src = InternalFS.open(SESSION_EVENTS_FILE, FILE_O_READ);
    if (!src) return;
    uint32_t fileSize = (uint32_t)src.size();
    if (fileSize == 0) { src.close(); return; }

    uint32_t activeTsBuf[MAX_SESSIONS];
    int activeCount = 0;
    collectActiveTimestamps(activeTsBuf, activeCount, MAX_SESSIONS);

    InternalFS.remove(kEvTempPath);
    File dst = InternalFS.open(kEvTempPath, FILE_O_WRITE);
    if (!dst) { src.close(); return; }

    uint8_t buf[128];
    uint32_t pos = 0;
    while (pos + sizeof(EventRecordHeader) <= fileSize) {
        EventRecordHeader hdr{};
        if (src.read((uint8_t*)&hdr, sizeof(hdr)) != (int)sizeof(hdr)) break;
        if (hdr.magic != SESSION_EVENT_MAGIC) break;
        uint32_t recEnd = pos + sizeof(hdr) + hdr.payloadLen;
        if (recEnd > fileSize) break;

        bool keep = isTimestampActive(hdr.session_ts, activeTsBuf, activeCount);
        if (keep) {
            dst.write((uint8_t*)&hdr, sizeof(hdr));
            uint16_t remaining = hdr.payloadLen;
            while (remaining > 0) {
                uint16_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
                int n = src.read(buf, chunk);
                if (n <= 0) break;
                dst.write(buf, (size_t)n);
                remaining -= (uint16_t)n;
            }
        }

        pos = recEnd;
        if (!src.seek(pos)) break;
    }
    src.close();
    dst.flush();
    dst.close();

    InternalFS.remove(SESSION_EVENTS_FILE);
    if (!InternalFS.rename(kEvTempPath, SESSION_EVENTS_FILE)) {
        File src2 = InternalFS.open(kEvTempPath, FILE_O_READ);
        File dst2 = InternalFS.open(SESSION_EVENTS_FILE, FILE_O_WRITE);
        if (src2 && dst2) {
            while (true) {
                int n = src2.read(buf, sizeof(buf));
                if (n <= 0) break;
                dst2.write(buf, (size_t)n);
            }
        }
        if (src2) src2.close();
        if (dst2) { dst2.flush(); dst2.close(); }
        InternalFS.remove(kEvTempPath);
    }
}

#endif  // SESSION_LOG_HAS_FS

void ensureReady() {
    if (g_ready) return;
#if SESSION_LOG_HAS_FS
    InternalFS.begin();
    loadFromDisk();
#endif
    g_ready = true;
}

}  // namespace

void session_log_init() {
    ensureReady();
}

void session_log_append(const StoredSession& s) {
    ensureReady();

    StoredSession copy = s;
    copy.sent = false;
    addSessionToRam(copy);

#if SESSION_LOG_HAS_FS
    // Queued and flushed from session_log_loop() shortly after — the mode
    // switch that ended this session never waits on flash.
    queueRecord(kRecSession, &copy, sizeof(copy));
#endif
}

int session_log_count_unsent() {
    ensureReady();
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_sessions[i].sent) n++;
    }
    return n;
}

bool session_log_get_unsent(int index, StoredSession& out, int& fileIndex) {
    ensureReady();
    if (index < 0) return false;

    int seen = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_sessions[i].sent) continue;
        if (seen == index) {
            out = g_sessions[i];
            fileIndex = i;
            return true;
        }
        seen++;
    }
    return false;
}

void session_log_mark_sent(int fileIndex) {
    ensureReady();
    if (fileIndex < 0 || fileIndex >= g_count) return;
    if (g_sessions[fileIndex].sent) return;

    g_sessions[fileIndex].sent = true;
#if SESSION_LOG_HAS_FS
    // Durable as a tiny marker append. Sessions with start_ts == 0 can't be
    // identified in the log; they simply get re-sent after a reboot and the
    // app-side upsert dedupes.
    if (g_sessions[fileIndex].start_ts != 0) {
        const uint32_t ts = g_sessions[fileIndex].start_ts;
        queueRecord(kRecSentMark, &ts, sizeof(ts));
    }
#endif
}

bool session_log_mark_sent_matching(const StoredSession& s) {
    ensureReady();
    for (int i = 0; i < g_count; i++) {
        const StoredSession& cur = g_sessions[i];
        if (cur.sent) continue;
        if (cur.type == s.type &&
            cur.start_ts == s.start_ts &&
            cur.duration_sec == s.duration_sec &&
            cur.wrong_count == s.wrong_count &&
            cur.wrong_dur_sec == s.wrong_dur_sec) {
            session_log_mark_sent(i);
            return true;
        }
    }
    return false;
}

void session_log_purge_sent() {
    ensureReady();

    int write = 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_sessions[i].sent) {
            if (write != i) {
                g_sessions[write] = g_sessions[i];
            }
            write++;
        }
    }

    if (write == g_count) return;

    g_count = write;
#if SESSION_LOG_HAS_FS
    // Runs at sync end, when nearly everything is sent — the compacted log
    // is close to empty, so this rewrite is at its cheapest.
    rewriteLogCompact();
    purgeOrphanedEvents();
#endif
}

void session_log_loop() {
#if SESSION_LOG_HAS_FS
    if (!g_ready || g_pendingLen == 0) return;
    if (millis() - g_pendingSinceMs < kFlushDelayMs) return;
    flushPending();
#endif
}

void session_log_write_training_events(uint32_t sessionTs,
                                       const uint16_t* slouchBuf,
                                       const uint16_t* correctionBuf,
                                       uint16_t slouchCount,
                                       uint16_t correctionCount) {
#if SESSION_LOG_HAS_FS
    ensureReady();
    if (sessionTs == 0) return;

    uint8_t pairCount = (slouchCount > MAX_POSTURE_EVENTS)
                        ? MAX_POSTURE_EVENTS : (uint8_t)slouchCount;
    if (pairCount == 0) return;

    uint16_t payloadLen = (uint16_t)pairCount * 4u;

    EventRecordHeader hdr{};
    hdr.magic      = SESSION_EVENT_MAGIC;
    hdr.type       = SESSION_TYPE_POSTURE;
    hdr.count      = pairCount;
    hdr.session_ts = sessionTs;
    hdr.payloadLen = payloadLen;

    uint8_t buf[sizeof(EventRecordHeader) + MAX_POSTURE_EVENTS * 4];
    memcpy(buf, &hdr, sizeof(hdr));
    uint8_t* p = buf + sizeof(hdr);
    for (uint8_t i = 0; i < pairCount; i++) {
        uint16_t sl = slouchBuf[i];
        uint16_t cr = (i < correctionCount) ? correctionBuf[i] : 0xFFFFu;
        p[0] = (uint8_t)(sl & 0xFF);
        p[1] = (uint8_t)((sl >> 8) & 0xFF);
        p[2] = (uint8_t)(cr & 0xFF);
        p[3] = (uint8_t)((cr >> 8) & 0xFF);
        p += 4;
    }

    bool ok = appendToEventFile(buf, sizeof(hdr) + payloadLen);
    rtt.printf("SESSION_EV: wrote %u posture pairs for ts=%lu (%s)\n",
                  (unsigned)pairCount, (unsigned long)sessionTs,
                  ok ? "OK" : "FAIL");
#else
    (void)sessionTs; (void)slouchBuf; (void)correctionBuf;
    (void)slouchCount; (void)correctionCount;
#endif
}

void session_log_write_therapy_events(uint32_t sessionTs,
                                      const uint8_t* patternBuf,
                                      uint8_t patternCount) {
#if SESSION_LOG_HAS_FS
    ensureReady();
    if (sessionTs == 0 || patternCount == 0) return;
    if (patternCount > MAX_THERAPY_PATTERNS) patternCount = MAX_THERAPY_PATTERNS;

    EventRecordHeader hdr{};
    hdr.magic      = SESSION_EVENT_MAGIC;
    hdr.type       = SESSION_TYPE_THERAPY;
    hdr.count      = patternCount;
    hdr.session_ts = sessionTs;
    hdr.payloadLen = patternCount;

    uint8_t buf[sizeof(EventRecordHeader) + MAX_THERAPY_PATTERNS];
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), patternBuf, patternCount);

    bool ok = appendToEventFile(buf, sizeof(hdr) + patternCount);
    rtt.printf("SESSION_EV: wrote %u therapy patterns for ts=%lu (%s)\n",
                  (unsigned)patternCount, (unsigned long)sessionTs,
                  ok ? "OK" : "FAIL");
#else
    (void)sessionTs; (void)patternBuf; (void)patternCount;
#endif
}

bool session_log_read_posture_events(uint32_t sessionTs,
                                     PostureEventReadResult& out) {
#if SESSION_LOG_HAS_FS
    ensureReady();
    EventRecordHeader hdr{};
    uint32_t payloadOffset = 0;
    if (!findEventRecord(sessionTs, hdr, payloadOffset)) return false;
    if (hdr.type != SESSION_TYPE_POSTURE || hdr.count == 0) return false;

    uint8_t pairs = hdr.count;
    if (pairs > MAX_POSTURE_EVENTS) pairs = MAX_POSTURE_EVENTS;

    File f = InternalFS.open(SESSION_EVENTS_FILE, FILE_O_READ);
    if (!f) return false;
    if (!f.seek(payloadOffset)) { f.close(); return false; }

    out.pairCount = pairs;
    for (uint8_t i = 0; i < pairs; i++) {
        uint8_t raw[4];
        if (f.read(raw, 4) != 4) { f.close(); return false; }
        out.slouchOffsets[i]     = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
        out.correctionOffsets[i] = (uint16_t)raw[2] | ((uint16_t)raw[3] << 8);
    }
    f.close();
    return true;
#else
    (void)sessionTs; (void)out;
    return false;
#endif
}

bool session_log_read_therapy_events(uint32_t sessionTs,
                                     TherapyEventReadResult& out) {
#if SESSION_LOG_HAS_FS
    ensureReady();
    EventRecordHeader hdr{};
    uint32_t payloadOffset = 0;
    if (!findEventRecord(sessionTs, hdr, payloadOffset)) return false;
    if (hdr.type != SESSION_TYPE_THERAPY || hdr.count == 0) return false;

    uint8_t cnt = hdr.count;
    if (cnt > MAX_THERAPY_PATTERNS) cnt = MAX_THERAPY_PATTERNS;

    File f = InternalFS.open(SESSION_EVENTS_FILE, FILE_O_READ);
    if (!f) return false;
    if (!f.seek(payloadOffset)) { f.close(); return false; }

    out.count = cnt;
    if (f.read(out.patterns, cnt) != (int)cnt) { f.close(); return false; }
    f.close();
    return true;
#else
    (void)sessionTs; (void)out;
    return false;
#endif
}
