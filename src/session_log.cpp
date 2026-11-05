#include "session_log.h"
#include "config.h"
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

constexpr const char* kTmpPath = "/sess_v2.tmp";

// Legacy (v1) files, migrated once at boot and then deleted.
constexpr const char* kLegacyLogPath   = "/sessions.log";
constexpr const char* kLegacyArrayPath = "/sessions.dat";
constexpr const char* kLegacyEvPath    = "/sess_ev.dat";
constexpr const char* kLegacyTmpPaths[] = { "/sessions.tmp", "/sess_ev.tmp" };

constexpr uint16_t kHeaderBytes = 18;
constexpr uint16_t kFooterBytes = 12;
constexpr uint16_t kSackBytes   = 12;
constexpr uint16_t kMaxRecordBytes =
    kHeaderBytes + 2 + MAX_STORED_EVENTS * 4 + 8 + kFooterBytes;

// The whole LittleFS partition is 28 KB (shared with every other file) and
// compaction briefly needs the old log plus its replacement (~18 KB peak),
// so live records are capped below a third of it. Roughly 100 typical
// sessions (~80 B each) or 27 worst-case ones (64 events, 296 B) fit.
constexpr uint32_t kMaxLiveBytes    = 8192u;  // indexed records (evict beyond)
constexpr uint32_t kLogCompactBytes = 9216u;  // file size that forces compaction
constexpr uint32_t kFlushDelayMs    = 400u;
constexpr uint32_t kRetryBackoffMs  = 5000u;

SessionIndexEntry g_index[MAX_SESSIONS];
int      g_count       = 0;
bool     g_ready       = false;
uint32_t g_fileSize    = 0;      // bytes of SESSION_LOG_FILE known to be on flash
bool     g_needCompact = false;  // dead/torn bytes in the file, or a failed append

// Pending appends are buffered in RAM and flushed from session_log_loop(),
// so a session ending on a button press never blocks on flash. A queued
// record's file_off is where it will land: g_fileSize + its buffer offset.
uint8_t  g_pendingBuf[1024];
size_t   g_pendingLen     = 0;
uint32_t g_pendingSinceMs = 0;
uint32_t g_nextAttemptMs  = 0;

uint8_t  g_scratch[kMaxRecordBytes];   // reads, replay, compaction
uint8_t  g_encodeBuf[kMaxRecordBytes]; // new records (a queue flush may compact)

// ── Byte helpers (little-endian) ─────────────────────────────────────────

inline void put16(uint8_t*& p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p += 2; }
inline void put32(uint8_t*& p, uint32_t v) { put16(p, (uint16_t)v); put16(p, (uint16_t)(v >> 16)); }
inline uint16_t get16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t get32(const uint8_t* p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// ── Record encode / decode ───────────────────────────────────────────────

uint16_t encodeRecord(const SessionRecord& r, uint8_t* buf) {
    uint8_t* p = buf + kHeaderBytes;
    if (r.type == SESSION_TYPE_POSTURE) {
        uint16_t n = r.event_count > MAX_STORED_EVENTS ? MAX_STORED_EVENTS : r.event_count;
        put16(p, n);
        for (uint16_t i = 0; i < n; i++) {
            put16(p, r.events[i].start_off);
            put16(p, r.events[i].dur);
        }
        put16(p, r.total_sec);
        put16(p, r.slouch_count);
        put16(p, r.bad_sec);
        put16(p, r.good_sec);
    } else {
        uint8_t n = r.event_count > MAX_THERAPY_PATTERNS ? MAX_THERAPY_PATTERNS : (uint8_t)r.event_count;
        *p++ = n;
        for (uint8_t i = 0; i < n; i++) {
            *p++ = r.events[i].pattern;
            put16(p, r.events[i].start_off);
            put16(p, r.events[i].dur);
        }
        put16(p, r.total_sec);
        *p++ = r.pattern_count;
        *p++ = 0;
    }
    const uint16_t bodyLen = (uint16_t)(p - (buf + kHeaderBytes));

    uint8_t* h = buf;
    put32(h, SREC_MAGIC);
    *h++ = SREC_VERSION;
    *h++ = r.type;
    *h++ = r.flags;
    *h++ = 0;
    put32(h, r.session_id);
    put32(h, r.start_ts);
    put16(h, bodyLen);

    const uint32_t crc = crc32(buf, kHeaderBytes + bodyLen);
    put32(p, SEND_MAGIC);
    put32(p, r.session_id);
    put32(p, crc);
    return (uint16_t)(p - buf);
}

// Checks magics, ids and CRC of a complete raw record of `len` bytes.
bool verifyRecord(const uint8_t* buf, uint16_t len) {
    if (len < kHeaderBytes + kFooterBytes) return false;
    if (get32(buf) != SREC_MAGIC || buf[4] != SREC_VERSION) return false;
    if (buf[5] != SESSION_TYPE_POSTURE && buf[5] != SESSION_TYPE_THERAPY) return false;
    if (kHeaderBytes + get16(buf + 16) + kFooterBytes != len) return false;
    const uint8_t* f = buf + len - kFooterBytes;
    return get32(f) == SEND_MAGIC &&
           get32(f + 4) == get32(buf + 8) &&
           get32(f + 8) == crc32(buf, len - kFooterBytes);
}

bool decodeRecord(const uint8_t* buf, uint16_t len, SessionRecord& r) {
    if (!verifyRecord(buf, len)) return false;
    memset(&r, 0, sizeof(r));
    r.type       = buf[5];
    r.flags      = buf[6];
    r.session_id = get32(buf + 8);
    r.start_ts   = get32(buf + 12);
    const uint16_t bodyLen = get16(buf + 16);
    const uint8_t* p = buf + kHeaderBytes;

    if (r.type == SESSION_TYPE_POSTURE) {
        uint16_t n = get16(p); p += 2;
        if (n > MAX_STORED_EVENTS || bodyLen != 2 + n * 4 + 8) return false;
        r.event_count = n;
        for (uint16_t i = 0; i < n; i++, p += 4) {
            r.events[i].start_off = get16(p);
            r.events[i].dur       = get16(p + 2);
        }
        r.total_sec    = get16(p);
        r.slouch_count = get16(p + 2);
        r.bad_sec      = get16(p + 4);
        r.good_sec     = get16(p + 6);
    } else {
        uint8_t n = *p++;
        if (n > MAX_THERAPY_PATTERNS || bodyLen != 1 + n * 5 + 4) return false;
        r.event_count = n;
        for (uint8_t i = 0; i < n; i++, p += 5) {
            r.events[i].pattern   = p[0];
            r.events[i].start_off = get16(p + 1);
            r.events[i].dur       = get16(p + 3);
        }
        r.total_sec     = get16(p);
        r.pattern_count = p[2];
    }
    return true;
}

// ── RAM index ────────────────────────────────────────────────────────────

uint32_t liveBytes() {
    uint32_t total = 0;
    for (int i = 0; i < g_count; i++) total += g_index[i].len;
    return total;
}

void removeIndex(int idx) {
    for (int i = idx; i < g_count - 1; i++) g_index[i] = g_index[i + 1];
    g_count--;
}

// Adds an entry, evicting the oldest sent record first (else the oldest
// record) to stay inside the count and byte budgets. Evicted bytes remain
// in the file until the next compaction.
void addIndex(const SessionIndexEntry& e) {
    while (g_count > 0 &&
           (g_count >= MAX_SESSIONS || liveBytes() + e.len > kMaxLiveBytes)) {
        int victim = 0;
        for (int i = 0; i < g_count; i++) {
            if (g_index[i].sent) { victim = i; break; }
        }
        rtt.printf("SESSION_LOG: evicting id=%lu (%s)\n",
                   (unsigned long)g_index[victim].session_id,
                   g_index[victim].sent ? "sent" : "UNSENT");
        removeIndex(victim);
        g_needCompact = true;
    }
    g_index[g_count++] = e;
}

int findIndex(uint32_t id) {
    for (int i = g_count - 1; i >= 0; i--) {
        if (g_index[i].session_id == id) return i;
    }
    return -1;
}

#if SESSION_LOG_HAS_FS

// Copies an entry's raw bytes from the file, or from the pending buffer if
// it has not been flushed yet.
bool readRaw(const SessionIndexEntry& e, uint8_t* buf) {
    if (e.len > kMaxRecordBytes) return false;
    if (e.file_off >= g_fileSize) {
        const uint32_t pOff = e.file_off - g_fileSize;
        if (pOff + e.len > g_pendingLen) return false;
        memcpy(buf, g_pendingBuf + pOff, e.len);
        return true;
    }
    File f = InternalFS.open(SESSION_LOG_FILE, FILE_O_READ);
    if (!f) return false;
    const bool ok = f.seek(e.file_off) && f.read(buf, e.len) == (int)e.len;
    f.close();
    return ok;
}

bool replaceLogWithTmp() {
    InternalFS.remove(SESSION_LOG_FILE);
    if (InternalFS.rename(kTmpPath, SESSION_LOG_FILE)) return true;

    File src = InternalFS.open(kTmpPath, FILE_O_READ);
    File dst = InternalFS.open(SESSION_LOG_FILE, FILE_O_WRITE);
    bool ok = src && dst;
    while (ok) {
        int n = src.read(g_scratch, sizeof(g_scratch));
        if (n <= 0) break;
        ok = dst.write(g_scratch, (size_t)n) == (size_t)n;
    }
    if (src) src.close();
    if (dst) dst.close();
    if (ok) InternalFS.remove(kTmpPath);
    return ok;
}

// Rewrites the log as just the unsent records (in order) via a temp file.
// On any failure the existing log and the pending queue are left untouched.
bool compact() {
    const uint32_t t0 = millis();
    InternalFS.remove(kTmpPath);
    File dst = InternalFS.open(kTmpPath, FILE_O_WRITE);
    if (!dst) return false;

    bool ok = true;
    for (int i = 0; i < g_count && ok; i++) {
        if (g_index[i].sent) continue;
        ok = readRaw(g_index[i], g_scratch) &&
             dst.write(g_scratch, g_index[i].len) == g_index[i].len;
    }
    dst.close();
    if (!ok || !replaceLogWithTmp()) {
        InternalFS.remove(kTmpPath);
        s_rttTiming.println("[SESSION_LOG] compaction FAILED, log kept");
        return false;
    }

    int w = 0;
    uint32_t off = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_index[i].sent) continue;
        SessionIndexEntry e = g_index[i];
        e.file_off = off;
        off += e.len;
        g_index[w++] = e;
    }
    g_count       = w;
    g_fileSize    = off;
    g_pendingLen  = 0;  // queued records are in the snapshot; markers are moot
    g_needCompact = false;

    s_rttTiming.print("[TIMING] session log compact: ");
    s_rttTiming.print(millis() - t0);
    s_rttTiming.println(" ms");
    return true;
}

// Returns true if everything queued is now on flash.
bool flushPending() {
    if (g_needCompact) return compact();
    if (g_pendingLen == 0) return true;
    const uint32_t t0 = millis();

    File f = InternalFS.open(SESSION_LOG_FILE, FILE_O_WRITE);  // positioned at end
    if (!f) return false;
    if ((uint32_t)f.size() != g_fileSize) {
        f.close();
        g_needCompact = true;  // file and RAM disagree: rebuild from RAM
        return compact();
    }
    const size_t written = f.write(g_pendingBuf, g_pendingLen);
    f.close();

    if (written != g_pendingLen) {
        // A torn tail would hide every later append; rebuild instead.
        g_needCompact = true;
        return compact();
    }
    g_fileSize  += g_pendingLen;
    g_pendingLen = 0;

    s_rttTiming.print("[TIMING] session log append: ");
    s_rttTiming.print(millis() - t0);
    s_rttTiming.println(" ms");

    if (g_fileSize > kLogCompactBytes) compact();
    return true;
}

// Queues raw bytes; outOff receives the file offset they will land at.
bool queueBytes(const uint8_t* data, uint16_t len, uint32_t& outOff) {
    if (g_pendingLen + len > sizeof(g_pendingBuf)) {
        flushPending();  // synchronous fallback if the queue overflows
        if (g_pendingLen + len > sizeof(g_pendingBuf)) return false;
    }
    if (g_pendingLen == 0) g_pendingSinceMs = millis();
    outOff = g_fileSize + g_pendingLen;
    memcpy(g_pendingBuf + g_pendingLen, data, len);
    g_pendingLen += len;
    return true;
}

bool queueRecord(const SessionRecord& rec) {
    const uint16_t len = encodeRecord(rec, g_encodeBuf);
    uint32_t off = 0;
    if (!queueBytes(g_encodeBuf, len, off)) return false;

    SessionIndexEntry e{};
    e.session_id = rec.session_id;
    e.start_ts   = rec.start_ts;
    e.file_off   = off;
    e.len        = len;
    e.type       = rec.type;
    e.sent       = false;
    addIndex(e);
    return true;
}

// Rebuilds the index by scanning the log. Records failing their CRC/footer
// check are skipped; an unreadable header or torn tail ends the scan. Either
// way the file is compacted so later appends never sit behind bad bytes.
void replayLog() {
    g_fileSize = 0;
    File f = InternalFS.open(SESSION_LOG_FILE, FILE_O_READ);
    if (!f) return;

    const uint32_t size = (uint32_t)f.size();
    uint32_t pos = 0;
    int skipped = 0;
    while (pos + 4 <= size) {
        if (!f.seek(pos) || f.read(g_scratch, 4) != 4) break;
        const uint32_t magic = get32(g_scratch);

        if (magic == SACK_MAGIC) {
            if (pos + kSackBytes > size) break;
            if (f.read(g_scratch + 4, kSackBytes - 4) != kSackBytes - 4) break;
            if (get32(g_scratch + 8) == crc32(g_scratch, 8)) {
                const int i = findIndex(get32(g_scratch + 4));
                if (i >= 0) g_index[i].sent = true;
            } else {
                g_needCompact = true;
            }
            pos += kSackBytes;
            continue;
        }

        if (magic != SREC_MAGIC || pos + kHeaderBytes > size) break;
        if (f.read(g_scratch + 4, kHeaderBytes - 4) != kHeaderBytes - 4) break;
        const uint32_t len = kHeaderBytes + get16(g_scratch + 16) + kFooterBytes;
        if (len > kMaxRecordBytes || pos + len > size) break;
        if (f.read(g_scratch + kHeaderBytes, len - kHeaderBytes) != (int)(len - kHeaderBytes)) break;

        if (verifyRecord(g_scratch, (uint16_t)len)) {
            SessionIndexEntry e{};
            e.session_id = get32(g_scratch + 8);
            e.start_ts   = get32(g_scratch + 12);
            e.file_off   = pos;
            e.len        = (uint16_t)len;
            e.type       = g_scratch[5];
            e.sent       = false;
            addIndex(e);
        } else {
            skipped++;
            g_needCompact = true;
        }
        pos += len;
    }
    f.close();

    if (pos != size) g_needCompact = true;  // torn or unknown tail
    g_fileSize = size;
    rtt.printf("SESSION_LOG: replayed %d records (%d bad, tail %s)\n",
               g_count, skipped, pos == size ? "clean" : "TORN");
}

// ── Legacy (v1) migration ────────────────────────────────────────────────

struct LegacySession {  // v1 StoredSession, unpacked layout as stored
    uint8_t  type;
    uint32_t start_ts;
    bool     ts_synced;
    uint16_t duration_sec;
    uint16_t wrong_count;
    uint16_t wrong_dur_sec;
    uint8_t  therapy_pattern;
    bool     sent;
};

struct __attribute__((packed)) LegacyEvHeader {
    uint32_t magic;  // "SEV1"
    uint8_t  type;
    uint8_t  count;
    uint32_t session_ts;
    uint16_t payloadLen;
};
constexpr uint32_t kLegacyEvMagic = 0x53455631u;

void legacyAdd(LegacySession* list, int& n, const LegacySession& s) {
    if (n >= MAX_SESSIONS) {
        int victim = 0;
        for (int i = 0; i < n; i++) {
            if (list[i].sent) { victim = i; break; }
        }
        for (int i = victim; i < n - 1; i++) list[i] = list[i + 1];
        n--;
    }
    list[n++] = s;
}

int loadLegacySessions(LegacySession* list) {
    int n = 0;
    File f = InternalFS.open(kLegacyLogPath, FILE_O_READ);
    if (f) {
        while (true) {
            uint8_t kind = 0;
            if (f.read(&kind, 1) != 1) break;
            if (kind == 0xA5) {
                LegacySession s{};
                if (f.read((uint8_t*)&s, sizeof(s)) != (int)sizeof(s)) break;
                if (s.type != SESSION_TYPE_POSTURE && s.type != SESSION_TYPE_THERAPY) break;
                legacyAdd(list, n, s);
            } else if (kind == 0x5A) {
                uint32_t ts = 0;
                if (f.read((uint8_t*)&ts, sizeof(ts)) != (int)sizeof(ts)) break;
                for (int i = 0; i < n; i++) {
                    if (list[i].start_ts == ts) list[i].sent = true;
                }
            } else {
                break;
            }
        }
        f.close();
        return n;
    }

    f = InternalFS.open(kLegacyArrayPath, FILE_O_READ);
    if (!f) return 0;
    LegacySession s{};
    while (n < MAX_SESSIONS && f.read((uint8_t*)&s, sizeof(s)) == (int)sizeof(s)) {
        if (s.type == SESSION_TYPE_POSTURE || s.type == SESSION_TYPE_THERAPY) list[n++] = s;
    }
    f.close();
    return n;
}

// Reads the legacy event payload for (ts, type) into `out`; returns its count.
uint8_t loadLegacyEvents(uint32_t ts, uint8_t type, uint8_t* out, uint16_t maxBytes,
                         uint16_t& payloadLen) {
    payloadLen = 0;
    if (ts == 0) return 0;
    File f = InternalFS.open(kLegacyEvPath, FILE_O_READ);
    if (!f) return 0;
    const uint32_t size = (uint32_t)f.size();
    uint32_t pos = 0;
    uint8_t count = 0;
    while (pos + sizeof(LegacyEvHeader) <= size) {
        LegacyEvHeader h{};
        if (!f.seek(pos) || f.read((uint8_t*)&h, sizeof(h)) != (int)sizeof(h)) break;
        if (h.magic != kLegacyEvMagic) break;
        const uint32_t end = pos + sizeof(h) + h.payloadLen;
        if (end > size) break;
        if (h.session_ts == ts && h.type == type && h.payloadLen <= maxBytes) {
            if (f.read(out, h.payloadLen) == (int)h.payloadLen) {
                count = h.count;
                payloadLen = h.payloadLen;
            }
            break;
        }
        pos = end;
    }
    f.close();
    return count;
}

bool alreadyStored(uint8_t type, uint32_t ts) {
    for (int i = 0; i < g_count; i++) {
        if (g_index[i].type == type && g_index[i].start_ts == ts) return true;
    }
    return false;
}

void migrateLegacy() {
    const bool hasLegacy = InternalFS.exists(kLegacyLogPath) ||
                           InternalFS.exists(kLegacyArrayPath) ||
                           InternalFS.exists(kLegacyEvPath);
    if (!hasLegacy) return;

    LegacySession* list = new LegacySession[MAX_SESSIONS];
    if (!list) return;
    const int n = loadLegacySessions(list);

    static SessionRecord rec;
    uint8_t evBuf[MAX_STORED_EVENTS * 4];
    uint32_t nextId = session_log_max_id() + 1;
    int migrated = 0;
    bool ok = true;

    for (int i = 0; i < n && ok; i++) {
        const LegacySession& s = list[i];
        if (s.sent) continue;
        if (s.start_ts != 0 && alreadyStored(s.type, s.start_ts)) continue;  // re-run after crash

        memset(&rec, 0, sizeof(rec));
        rec.type       = s.type;
        rec.flags      = s.ts_synced ? SREC_FLAG_TIME_SYNCED : 0;
        rec.session_id = nextId++;
        rec.start_ts   = s.start_ts;
        rec.total_sec  = s.duration_sec;

        uint16_t payloadLen = 0;
        const uint8_t cnt = loadLegacyEvents(s.start_ts, s.type, evBuf, sizeof(evBuf), payloadLen);
        if (s.type == SESSION_TYPE_POSTURE) {
            const uint16_t pairs = (cnt * 4u <= payloadLen) ? cnt : payloadLen / 4u;
            for (uint16_t k = 0; k < pairs && k < MAX_STORED_EVENTS; k++) {
                const uint16_t sl = get16(evBuf + k * 4);
                const uint16_t cr = get16(evBuf + k * 4 + 2);
                SessionEvent& ev = rec.events[rec.event_count++];
                ev.start_off = sl;
                if (cr != 0xFFFFu && cr >= sl)      ev.dur = cr - sl;
                else if (rec.total_sec > sl)        ev.dur = rec.total_sec - sl;
            }
            rec.slouch_count = s.wrong_count;
            rec.bad_sec      = s.wrong_dur_sec > rec.total_sec ? rec.total_sec : s.wrong_dur_sec;
            rec.good_sec     = rec.total_sec - rec.bad_sec;
            if (s.wrong_count > rec.event_count) rec.flags |= SREC_FLAG_EVENTS_TRUNCATED;
        } else {
            const uint8_t pats = cnt <= payloadLen ? cnt : (uint8_t)payloadLen;
            session_record_fill_therapy(rec, evBuf, pats,
                                        (uint16_t)(THERAPY_PATTERN_MS / 1000UL));
        }
        ok = queueRecord(rec);
        if (ok) migrated++;
    }
    delete[] list;

    if (ok && flushPending() && !g_needCompact && g_pendingLen == 0) {
        InternalFS.remove(kLegacyLogPath);
        InternalFS.remove(kLegacyArrayPath);
        InternalFS.remove(kLegacyEvPath);
        for (const char* p : kLegacyTmpPaths) InternalFS.remove(p);
        rtt.printf("SESSION_LOG: migrated %d legacy sessions\n", migrated);
    } else {
        rtt.println("SESSION_LOG: legacy migration incomplete, will retry next boot");
    }
}

void loadFromDisk() {
    g_count = 0;
    // A compaction interrupted after the old log was removed leaves only the
    // (complete) temp file; otherwise a leftover temp is an aborted write.
    if (!InternalFS.exists(SESSION_LOG_FILE) && InternalFS.exists(kTmpPath)) {
        InternalFS.rename(kTmpPath, SESSION_LOG_FILE);
    } else {
        InternalFS.remove(kTmpPath);
    }
    replayLog();
    if (g_needCompact) compact();
    migrateLegacy();
}

#endif  // SESSION_LOG_HAS_FS

void ensureReady() {
    if (g_ready) return;
    g_ready = true;
#if SESSION_LOG_HAS_FS
    InternalFS.begin();
    loadFromDisk();
#endif
}

}  // namespace

void session_record_fill_therapy(SessionRecord& r, const uint8_t* patterns,
                                 uint8_t count, uint16_t patternSec) {
    if (count > MAX_THERAPY_PATTERNS) count = MAX_THERAPY_PATTERNS;
    r.pattern_count = count;
    r.event_count   = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint32_t start = (uint32_t)i * patternSec;
        const uint32_t left  = r.total_sec > start ? r.total_sec - start : 0u;
        SessionEvent& ev = r.events[i];
        ev.pattern   = patterns[i];
        ev.start_off = start > 0xFFFFu ? 0xFFFFu : (uint16_t)start;
        // The last pattern runs until the session ends; earlier ones ran in full.
        ev.dur = (uint16_t)((i == count - 1 || left < patternSec) ? left : patternSec);
    }
}

void session_log_init() {
    ensureReady();
}

bool session_log_append(const SessionRecord& rec) {
    ensureReady();
#if SESSION_LOG_HAS_FS
    // Queued and flushed from session_log_loop() shortly after — the mode
    // switch that ended this session never waits on flash.
    return queueRecord(rec);
#else
    (void)rec;
    return false;
#endif
}

int session_log_count_unsent() {
    ensureReady();
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_index[i].sent) n++;
    }
    return n;
}

bool session_log_get_unsent(int index, SessionIndexEntry& out) {
    ensureReady();
    if (index < 0) return false;
    int seen = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_index[i].sent) continue;
        if (seen == index) {
            out = g_index[i];
            return true;
        }
        seen++;
    }
    return false;
}

bool session_log_read(uint32_t sessionId, SessionRecord& out) {
    ensureReady();
#if SESSION_LOG_HAS_FS
    const int i = findIndex(sessionId);
    if (i < 0) return false;
    return readRaw(g_index[i], g_scratch) &&
           decodeRecord(g_scratch, g_index[i].len, out);
#else
    (void)sessionId; (void)out;
    return false;
#endif
}

void session_log_mark_sent(uint32_t sessionId) {
    ensureReady();
    const int i = findIndex(sessionId);
    if (i < 0 || g_index[i].sent) return;
    g_index[i].sent = true;
#if SESSION_LOG_HAS_FS
    uint8_t marker[kSackBytes];
    uint8_t* p = marker;
    put32(p, SACK_MAGIC);
    put32(p, sessionId);
    put32(p, crc32(marker, 8));
    uint32_t off = 0;
    queueBytes(marker, kSackBytes, off);
#endif
}

void session_log_purge_sent() {
    ensureReady();
    bool anySent = false;
    for (int i = 0; i < g_count; i++) {
        if (g_index[i].sent) { anySent = true; break; }
    }
#if SESSION_LOG_HAS_FS
    // Runs at sync end, when nearly everything is sent — the compacted log
    // is close to empty, so this rewrite is at its cheapest.
    if (anySent || g_needCompact) compact();
#else
    (void)anySent;
#endif
}

uint32_t session_log_max_id() {
    uint32_t maxId = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_index[i].session_id > maxId) maxId = g_index[i].session_id;
    }
    return maxId;
}

void session_log_loop() {
#if SESSION_LOG_HAS_FS
    if (!g_ready || (g_pendingLen == 0 && !g_needCompact)) return;
    const uint32_t now = millis();
    if (g_pendingLen > 0 && now - g_pendingSinceMs < kFlushDelayMs) return;
    if ((int32_t)(now - g_nextAttemptMs) < 0) return;
    if (!flushPending()) g_nextAttemptMs = now + kRetryBackoffMs;
#endif
}
