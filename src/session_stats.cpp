#include "session_stats.h"
#include "config.h"
#include "device_time.h"
#include "therapy.h"
#include "session_log.h"
#include "bluetooth.h"
#include <stdio.h>
#include <stdarg.h>
#include <RTTStream.h>

extern RTTStream rtt;

#if __has_include(<InternalFileSystem.h>)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#define SESSION_HAS_FS 1
#else
#define SESSION_HAS_FS 0
#endif

extern bool isBadPosture;

static void serialPrintf(const char* fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    rtt.print(buf);
}

static const unsigned long SESSION_PROMOTE_MS = 30000UL;
static const uint16_t MAX_EVENTS_PER_SESSION = 512u;

static const uint32_t STATE_MAGIC   = 0x53455332u; // "SES2"
static const uint16_t STATE_VERSION = 1u;
static const char *STATE_PATH       = "/sess.st";
static const char *STATE_TMP_PATH   = "/sess.st.tmp";

struct __attribute__((packed)) PersistedState {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t nextSessionId;
    uint32_t trainingSessionCount;
    uint32_t therapySessionCount;
    uint32_t lastTrainingStartEpoch;
    uint32_t lastTrainingEndEpoch;
    uint32_t lastTherapyStartEpoch;
    uint32_t lastTherapyEndEpoch;
};

static PersistedState state = {
    STATE_MAGIC,
    STATE_VERSION,
    0u,
    1u,
    0u, 0u,
    0u, 0u,
    0u, 0u
};

static bool stateLoaded = false;

static bool     trainingActive         = false;
static bool     trainingPromoted       = false;
static unsigned long trainingEnteredMs = 0;
static uint64_t trainingStartTicks     = 0;
static uint8_t  trainingSubMode        = (uint8_t)SUBMODE_INSTANT;
static uint32_t trainingCurrentId      = 0;

static bool     wasBadPosturePrev      = false;
static uint16_t slouchBuf[MAX_EVENTS_PER_SESSION];
static uint16_t correctionBuf[MAX_EVENTS_PER_SESSION];
static uint16_t slouchCount            = 0;
static uint16_t correctionCount        = 0;
static bool     eventBufferOverflowed  = false;

static bool     therapyActive          = false;
static bool     therapyPromoted        = false;
static unsigned long therapyEnteredMs  = 0;
static uint64_t therapyStartTicks      = 0;
static uint8_t  therapySubMode         = 0;
static uint32_t therapyCurrentId       = 0;

static DeviceTimeStatus lastSeenTimeStatus = TIME_UNKNOWN;

static uint8_t trainingDelayToSubMode(TrainingDelay d) {
    switch (d) {
        case TRAIN_INSTANT:   return (uint8_t)SUBMODE_INSTANT;
        case TRAIN_DELAYED:   return (uint8_t)SUBMODE_DELAYED;
        case TRAIN_AUTOMATIC: return (uint8_t)SUBMODE_AUTOMATIC;
    }
    return (uint8_t)SUBMODE_INSTANT;
}

static uint16_t clampToU16(unsigned long secs) {
    if (secs > 0xFFFFUL) return 0xFFFFu;
    return (uint16_t)secs;
}

#if SESSION_HAS_FS
static bool writeStateAtomic() {
    InternalFS.remove(STATE_TMP_PATH);
    File tmp = InternalFS.open(STATE_TMP_PATH, FILE_O_WRITE);
    if (!tmp) return false;
    size_t written = tmp.write((uint8_t*)&state, sizeof(state));
    tmp.flush();
    tmp.close();
    if (written != sizeof(state)) {
        InternalFS.remove(STATE_TMP_PATH);
        return false;
    }
    InternalFS.remove(STATE_PATH);
    if (!InternalFS.rename(STATE_TMP_PATH, STATE_PATH)) {
        File direct = InternalFS.open(STATE_PATH, FILE_O_WRITE);
        if (!direct) {
            InternalFS.remove(STATE_TMP_PATH);
            return false;
        }
        size_t w2 = direct.write((uint8_t*)&state, sizeof(state));
        direct.flush();
        direct.close();
        InternalFS.remove(STATE_TMP_PATH);
        if (w2 != sizeof(state)) return false;
    }
    return true;
}

static void loadState() {
    File file = InternalFS.open(STATE_PATH, FILE_O_READ);
    if (!file) return;
    PersistedState loaded{};
    if (file.read((uint8_t*)&loaded, sizeof(loaded)) == (int)sizeof(loaded)) {
        if (loaded.magic == STATE_MAGIC && loaded.version == STATE_VERSION) {
            state = loaded;
        }
    }
    file.close();
}

static bool saveState() {
    return writeStateAtomic();
}

#else
static void loadState() {}
static bool saveState() { return false; }
#endif

void initSessionStats() {
    if (!stateLoaded) {
        loadState();
        stateLoaded = true;
    }
    trainingActive         = false;
    trainingPromoted       = false;
    trainingCurrentId      = 0;
    wasBadPosturePrev      = false;
    slouchCount            = 0;
    correctionCount        = 0;
    eventBufferOverflowed  = false;

    therapyActive          = false;
    therapyPromoted        = false;
    therapyCurrentId       = 0;

    lastSeenTimeStatus     = getDeviceTimeStatus();

    serialPrintf("SESSION: state loaded. nextId=%lu, counts t/p=%lu/%lu, unsent=%d\n",
                 (unsigned long)state.nextSessionId,
                 (unsigned long)state.trainingSessionCount,
                 (unsigned long)state.therapySessionCount,
                 session_log_count_unsent());
}

void onTrainingStarted() {
    trainingActive        = true;
    trainingPromoted      = false;
    trainingEnteredMs     = millis();
    trainingStartTicks    = getDeviceTicks();
    trainingSubMode       = trainingDelayToSubMode(currentTrainingDelay);
    trainingCurrentId     = state.nextSessionId;
    slouchCount           = 0;
    correctionCount       = 0;
    eventBufferOverflowed = false;
    wasBadPosturePrev     = false;
    serialPrintf("SESSION: Training entered (pending id=%lu, subMode=%u)\n",
                 (unsigned long)trainingCurrentId, (unsigned)trainingSubMode);
}

static void finalizeTrainingRecord() {
    unsigned long elapsedMs = millis() - trainingEnteredMs;
    uint32_t durationSec    = (uint32_t)(elapsedMs / 1000UL);
    uint32_t nowEpoch       = getDeviceTime();
    uint32_t startEpoch     = 0;

    if (nowEpoch != 0 && nowEpoch >= durationSec) {
        startEpoch = nowEpoch - durationSec;
    } else {
        startEpoch = ticksToEpoch(trainingStartTicks);
    }

    state.trainingSessionCount++;
    if (state.nextSessionId == trainingCurrentId) state.nextSessionId++;
    state.lastTrainingStartEpoch = startEpoch;
    state.lastTrainingEndEpoch   = getDeviceTime();
    saveState();

    uint32_t wrongDur = 0;
    uint16_t pairCount = correctionCount < slouchCount ? correctionCount : slouchCount;
    for (uint16_t i = 0; i < pairCount; i++) {
        if (correctionBuf[i] > slouchBuf[i]) {
            wrongDur += (uint32_t)(correctionBuf[i] - slouchBuf[i]);
        }
    }
    if (slouchCount > correctionCount) {
        uint16_t lastSlouch = slouchBuf[slouchCount - 1];
        if (durationSec > lastSlouch) {
            wrongDur += (durationSec - lastSlouch);
        }
    }

    {
        StoredSession rec{};
        rec.type             = SESSION_TYPE_POSTURE;
        rec.start_ts         = startEpoch;
        rec.ts_synced        = (startEpoch != 0 && getDeviceTimeStatus() == TIME_FRESH);
        rec.duration_sec     = clampToU16(durationSec);
        rec.wrong_count      = slouchCount;
        rec.wrong_dur_sec    = clampToU16(wrongDur);
        rec.therapy_pattern  = 0;
        rec.sent             = false;
        session_log_append(rec);

        session_log_write_training_events(
            startEpoch, slouchBuf, correctionBuf,
            slouchCount, correctionCount);
        notifyNewSessionStored();
    }

    char stampBuf[24];
    formatEpochUTC(startEpoch, stampBuf, sizeof(stampBuf));
    rtt.println("========================================");
    rtt.println("  TRAINING SESSION SAVED               ");
    serialPrintf ("  ID      : #%lu\n", (unsigned long)trainingCurrentId);
    serialPrintf ("  Start   : %s UTC\n", stampBuf);
    serialPrintf ("  Dur     : %lu s\n", (unsigned long)durationSec);
    serialPrintf ("  Slouches: %u  Corrections: %u%s\n",
                  (unsigned)slouchCount, (unsigned)correctionCount,
                  eventBufferOverflowed ? " (truncated)" : "");

    {
        const uint16_t printLimit = 16;
        uint16_t printed = 0;
        for (uint16_t i = 0; i < slouchCount && printed < printLimit; i++, printed++) {
            uint16_t s = slouchBuf[i];
            serialPrintf("  Event %u: Slouched at %um %02us",
                         (unsigned)(i + 1),
                         (unsigned)(s / 60), (unsigned)(s % 60));
            if (i < correctionCount) {
                uint16_t c = correctionBuf[i];
                serialPrintf(" -> Corrected at %um %02us (held %us)",
                             (unsigned)(c / 60), (unsigned)(c % 60),
                             (unsigned)(c > s ? (c - s) : 0));
            } else {
                rtt.print(" -> not corrected");
            }
            rtt.println();
        }
        if (slouchCount > printLimit) {
            serialPrintf("  ... (%u more events not printed)\n",
                         (unsigned)(slouchCount - printLimit));
        }
    }

    serialPrintf ("  Total stored sessions: %lu\n",
                  (unsigned long)state.trainingSessionCount);
    rtt.println("========================================");
}

void onTrainingEnded() {
    if (!trainingActive) return;

    if (trainingPromoted) {
        finalizeTrainingRecord();
    } else {
        unsigned long elapsedMs = millis() - trainingEnteredMs;
        rtt.println("----------------------------------------");
        serialPrintf ("  TRAINING DISCARDED (only %lu s < 30s)\n",
                      elapsedMs / 1000UL);
        rtt.println("----------------------------------------");
    }

    trainingActive        = false;
    trainingPromoted      = false;
    trainingCurrentId     = 0;
    slouchCount           = 0;
    correctionCount       = 0;
    eventBufferOverflowed = false;
    wasBadPosturePrev     = false;
}

void onTherapyStarted() {
    therapyActive      = true;
    therapyPromoted    = false;
    therapyEnteredMs   = millis();
    therapyStartTicks  = getDeviceTicks();
    therapySubMode     = (uint8_t)(therapyDuration / 60000UL);
    therapyCurrentId   = state.nextSessionId;
    serialPrintf("SESSION: Therapy entered (pending id=%lu, subMode=%u min)\n",
                 (unsigned long)therapyCurrentId, (unsigned)therapySubMode);
}

static void finalizeTherapyRecord() {
    unsigned long elapsedMs = millis() - therapyEnteredMs;
    uint32_t durationSec    = (uint32_t)(elapsedMs / 1000UL);
    uint32_t nowEpoch       = getDeviceTime();
    uint32_t startEpoch     = 0;

    if (nowEpoch != 0 && nowEpoch >= durationSec) {
        startEpoch = nowEpoch - durationSec;
    } else {
        startEpoch = ticksToEpoch(therapyStartTicks);
    }

    uint16_t uniquePatterns = getTherapyUniquePatternCount();
    uint16_t totalPatterns  = getTherapyTotalPatternCount();

    state.therapySessionCount++;
    if (state.nextSessionId == therapyCurrentId) state.nextSessionId++;
    state.lastTherapyStartEpoch = startEpoch;
    state.lastTherapyEndEpoch   = getDeviceTime();
    saveState();

    int patternIdx = currentPatternIndex;
    if (patternIdx < 0) patternIdx = 0;
    if (patternIdx > 255) patternIdx = 255;

    {
        StoredSession rec{};
        rec.type             = SESSION_TYPE_THERAPY;
        rec.start_ts         = startEpoch;
        rec.ts_synced        = (startEpoch != 0 && getDeviceTimeStatus() == TIME_FRESH);
        rec.duration_sec     = clampToU16(durationSec);
        rec.wrong_count      = 0;
        rec.wrong_dur_sec    = 0;
        rec.therapy_pattern  = (uint8_t)patternIdx;
        rec.sent             = false;
        session_log_append(rec);

        uint8_t seqBuf[MAX_THERAPY_PATTERNS];
        int seqLen = getTherapyPatternSequence(seqBuf, MAX_THERAPY_PATTERNS);
        if (seqLen > 0) {
            session_log_write_therapy_events(
                startEpoch, seqBuf, (uint8_t)seqLen);
        }
        notifyNewSessionStored();
    }

    char stampBuf[24];
    formatEpochUTC(startEpoch, stampBuf, sizeof(stampBuf));
    rtt.println("========================================");
    rtt.println("  THERAPY SESSION SAVED                ");
    serialPrintf ("  ID       : #%lu\n", (unsigned long)therapyCurrentId);
    serialPrintf ("  Start    : %s UTC\n", stampBuf);
    serialPrintf ("  Dur      : %lu s\n", (unsigned long)durationSec);
    serialPrintf ("  Duration : %u min\n", (unsigned)therapySubMode);
    serialPrintf ("  Patterns : %u unique / %u total\n",
                  (unsigned)uniquePatterns, (unsigned)totalPatterns);

    {
        uint8_t printSeq[MAX_THERAPY_PATTERNS];
        int printLen = getTherapyPatternSequence(printSeq, MAX_THERAPY_PATTERNS);
        if (printLen > 0) {
            for (int i = 0; i < printLen; i++) {
                uint32_t patDur = (uint32_t)(THERAPY_PATTERN_MS/1000UL);
                if (i == printLen - 1) {
                    uint32_t consumed = (uint32_t)(THERAPY_PATTERN_MS / 1000UL) * (uint32_t)(printLen - 1);
                    patDur = (durationSec > consumed) ? (durationSec - consumed) : 0u;
                    if (patDur == 0u) patDur = (uint32_t)(durationSec % 60u);
                    if (patDur == 0u) patDur = 60u;
                }
                serialPrintf("  Pattern %d: %-18s (%lus)\n",
                             i + 1,
                             getPatternNameByIndex((int)printSeq[i]),
                             (unsigned long)patDur);
            }
        }
    }

    serialPrintf ("  Total stored sessions: %lu\n",
                  (unsigned long)state.therapySessionCount);
    rtt.println("========================================");
}

void onTherapyEnded() {
    if (!therapyActive) return;

    if (therapyPromoted) {
        finalizeTherapyRecord();
    } else {
        unsigned long elapsedMs = millis() - therapyEnteredMs;
        rtt.println("----------------------------------------");
        serialPrintf ("  THERAPY DISCARDED (only %lu s < 30s)\n",
                      elapsedMs / 1000UL);
        rtt.println("----------------------------------------");
    }

    therapyActive     = false;
    therapyPromoted   = false;
    therapyCurrentId  = 0;
}

void updateSessionStats() {
    unsigned long nowMs = millis();

    if (trainingActive && !trainingPromoted) {
        if ((nowMs - trainingEnteredMs) >= SESSION_PROMOTE_MS) {
            trainingPromoted = true;
            rtt.println(">>> Training 30s reached - will be SAVED on exit <<<");
        }
    }

    if (trainingActive) {
        bool curBad = isBadPosture;
        if (curBad != wasBadPosturePrev) {
            unsigned long offsetSec = (nowMs - trainingEnteredMs) / 1000UL;
            uint16_t offset = clampToU16(offsetSec);
            if (curBad) {
                if (slouchCount < MAX_EVENTS_PER_SESSION) {
                    slouchBuf[slouchCount++] = offset;
                } else {
                    eventBufferOverflowed = true;
                }
            } else {
                if (correctionCount < MAX_EVENTS_PER_SESSION) {
                    correctionBuf[correctionCount++] = offset;
                } else {
                    eventBufferOverflowed = true;
                }
            }
            wasBadPosturePrev = curBad;
        }
    }

    if (therapyActive && !therapyPromoted) {
        if ((nowMs - therapyEnteredMs) >= SESSION_PROMOTE_MS) {
            therapyPromoted = true;
            rtt.println(">>> Therapy 30s reached - will be SAVED on exit <<<");
        }
    }
}

void maintainSessionStats() {
    DeviceTimeStatus s = getDeviceTimeStatus();
    if (s == TIME_UNKNOWN) {
        lastSeenTimeStatus = s;
        return;
    }

    bool dirty = false;

    if (state.lastTrainingStartEpoch == 0 && trainingActive && trainingPromoted) {
        uint32_t e = ticksToEpoch(trainingStartTicks);
        if (e != 0) {
            state.lastTrainingStartEpoch = e;
            dirty = true;
        }
    }
    if (state.lastTherapyStartEpoch == 0 && therapyActive && therapyPromoted) {
        uint32_t e = ticksToEpoch(therapyStartTicks);
        if (e != 0) {
            state.lastTherapyStartEpoch = e;
            dirty = true;
        }
    }

    if (dirty) saveState();
    lastSeenTimeStatus = s;
}

__attribute__((weak)) uint32_t getTrainingSessionNumber() {
    if (trainingActive) return trainingCurrentId;
    return state.trainingSessionCount;
}

__attribute__((weak)) uint32_t getTrainingSessionDurationSec() {
    if (!trainingActive) return 0;
    return (uint32_t)((millis() - trainingEnteredMs) / 1000UL);
}

__attribute__((weak)) uint32_t getTrainingSessionBadPostureCount() {
    return (uint32_t)slouchCount;
}

__attribute__((weak)) bool isTrainingSessionActive() {
    return trainingActive && trainingPromoted;
}

uint32_t getTherapySessionNumber() {
    if (therapyActive) return therapyCurrentId;
    return state.therapySessionCount;
}

uint32_t getTherapySessionDurationSec() {
    if (!therapyActive) return 0;
    return (uint32_t)((millis() - therapyEnteredMs) / 1000UL);
}

bool isTherapySessionActive() {
    return therapyActive && therapyPromoted;
}

bool isTrainingActive() {
    return trainingActive;
}

bool isTherapyActive() {
    return therapyActive;
}

uint32_t getActiveTrainingStartEpoch() {
    if (!trainingActive) return 0;
    uint32_t nowEpoch = getDeviceTime();
    uint32_t durationSec = (millis() - trainingEnteredMs) / 1000UL;
    if (nowEpoch != 0 && nowEpoch >= durationSec) {
        return nowEpoch - durationSec;
    }
    return ticksToEpoch(trainingStartTicks);
}

uint32_t getActiveTherapyStartEpoch() {
    if (!therapyActive) return 0;
    uint32_t nowEpoch = getDeviceTime();
    uint32_t durationSec = (millis() - therapyEnteredMs) / 1000UL;
    if (nowEpoch != 0 && nowEpoch >= durationSec) {
        return nowEpoch - durationSec;
    }
    return ticksToEpoch(therapyStartTicks);
}

uint32_t getLastTrainingStartEpoch() { return state.lastTrainingStartEpoch; }
uint32_t getLastTrainingEndEpoch()   { return state.lastTrainingEndEpoch; }
uint32_t getLastTherapyStartEpoch()  { return state.lastTherapyStartEpoch; }
uint32_t getLastTherapyEndEpoch()    { return state.lastTherapyEndEpoch; }

void resetAllSessionCounters() {
    state.nextSessionId          = 1u;
    state.trainingSessionCount   = 0u;
    state.therapySessionCount    = 0u;
    state.lastTrainingStartEpoch = 0u;
    state.lastTrainingEndEpoch   = 0u;
    state.lastTherapyStartEpoch  = 0u;
    state.lastTherapyEndEpoch    = 0u;
    saveState();

    slouchCount           = 0;
    correctionCount       = 0;
    eventBufferOverflowed = false;

    rtt.println("SESSION: All stats reset (counters zeroed)");
}
