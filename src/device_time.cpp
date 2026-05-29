#include "device_time.h"

#include <Arduino.h>
#include <time.h>
#include <stdlib.h>

#if defined(NRF52_SERIES) || defined(ARDUINO_NRF52_ADAFRUIT) || __has_include(<nrf.h>)
#include <nrf.h>
#define DEVTIME_HAS_RTC 1
#else
#define DEVTIME_HAS_RTC 0
#endif

#if __has_include(<InternalFileSystem.h>)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#define DEVTIME_HAS_FS 1
#else
#define DEVTIME_HAS_FS 0
#endif

static const uint32_t RTC2_PRESCALER     = 4095u;
static const uint32_t RTC2_TICKS_PER_SEC = 8u;

static const uint32_t AUTO_PERSIST_INTERVAL_TICKS =
    RTC2_TICKS_PER_SEC * 5u * 60u;

static const uint32_t MIN_PERSIST_DELTA_SECONDS = 30u;

static const uint32_t MIN_ACCEPTED_EPOCH = 1704067200u;
static const uint32_t MAX_ACCEPTED_EPOCH = 4102444800u;

static const uint32_t DEVTIME_MAGIC   = 0x44544D45u;
static const uint16_t DEVTIME_VERSION = 1u;
static const char *DEVTIME_PATH       = "/devtime.dat";
static const char *DEVTIME_TMP_PATH   = "/devtime.tmp";

static volatile uint32_t g_rtc2Overflows = 0;

static uint64_t g_baseTicks = 0;
static uint32_t g_baseEpoch = 0;

static uint64_t g_bootTicks = 0;

static uint64_t g_lastSyncTicks = UINT64_MAX;

static uint32_t g_lastPersistedEpoch = 0;
static uint64_t g_lastPersistTicks  = 0;

static DeviceTimeStatus g_status = TIME_UNKNOWN;
static bool g_initialized = false;

static int32_t g_tzOffsetSeconds = 0;

struct PersistedTime {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t epochSeconds;
    uint32_t reserved2;
};

#if DEVTIME_HAS_FS
static bool writePersistedAtomic(uint32_t epoch) {
    PersistedTime payload{};
    payload.magic = DEVTIME_MAGIC;
    payload.version = DEVTIME_VERSION;
    payload.epochSeconds = epoch;

    InternalFS.remove(DEVTIME_TMP_PATH);
    File tmp = InternalFS.open(DEVTIME_TMP_PATH, FILE_O_WRITE);
    if (!tmp) {
        return false;
    }
    size_t written = tmp.write((uint8_t*)&payload, sizeof(payload));
    tmp.flush();
    tmp.close();
    if (written != sizeof(payload)) {
        InternalFS.remove(DEVTIME_TMP_PATH);
        return false;
    }

    InternalFS.remove(DEVTIME_PATH);
    if (!InternalFS.rename(DEVTIME_TMP_PATH, DEVTIME_PATH)) {
        File direct = InternalFS.open(DEVTIME_PATH, FILE_O_WRITE);
        if (!direct) {
            InternalFS.remove(DEVTIME_TMP_PATH);
            return false;
        }
        size_t wrote2 = direct.write((uint8_t*)&payload, sizeof(payload));
        direct.flush();
        direct.close();
        InternalFS.remove(DEVTIME_TMP_PATH);
        if (wrote2 != sizeof(payload)) {
            return false;
        }
    }
    return true;
}

static void loadPersistedTime() {
    File file = InternalFS.open(DEVTIME_PATH, FILE_O_READ);
    if (!file) {
        return;
    }
    PersistedTime saved{};
    if (file.read(&saved, sizeof(saved)) == sizeof(saved)) {
        if (saved.magic == DEVTIME_MAGIC &&
            saved.version == DEVTIME_VERSION &&
            saved.epochSeconds >= MIN_ACCEPTED_EPOCH &&
            saved.epochSeconds <= MAX_ACCEPTED_EPOCH) {
            g_baseEpoch = saved.epochSeconds;
            g_baseTicks = 0;
            g_lastPersistedEpoch = saved.epochSeconds;
            g_status = TIME_STALE;
        }
    }
    file.close();
}

static bool savePersistedTime(uint32_t epoch, bool force) {
    if (epoch < MIN_ACCEPTED_EPOCH || epoch > MAX_ACCEPTED_EPOCH) {
        return false;
    }
    if (!force && g_lastPersistedEpoch != 0) {
        uint32_t delta = (epoch >= g_lastPersistedEpoch)
                            ? (epoch - g_lastPersistedEpoch)
                            : (g_lastPersistedEpoch - epoch);
        if (delta < MIN_PERSIST_DELTA_SECONDS) {
            return false;
        }
    }
    if (!writePersistedAtomic(epoch)) {
        return false;
    }
    g_lastPersistedEpoch = epoch;
    return true;
}
#else
static void loadPersistedTime() {}
static bool savePersistedTime(uint32_t, bool) { return false; }
#endif

#if DEVTIME_HAS_RTC
extern "C" void RTC2_IRQHandler(void) {
    if (NRF_RTC2->EVENTS_OVRFLW) {
        NRF_RTC2->EVENTS_OVRFLW = 0;
        (void)NRF_RTC2->EVENTS_OVRFLW;
        g_rtc2Overflows++;
    }
}

static int readLfclkSource() {
    uint32_t stat = NRF_CLOCK->LFCLKSTAT;
    if ((stat & CLOCK_LFCLKSTAT_STATE_Msk) == 0) {
        return -1;
    }
    uint32_t src = (stat & CLOCK_LFCLKSTAT_SRC_Msk) >> CLOCK_LFCLKSTAT_SRC_Pos;
    return (int)src;
}

static void startLfclkIfNeeded() {
    if ((NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk) != 0) {
        return;
    }
    NRF_CLOCK->LFCLKSRC = (CLOCK_LFCLKSRC_SRC_Xtal << CLOCK_LFCLKSRC_SRC_Pos);
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_LFCLKSTART = 1;
    uint32_t guard = 0;
    while (!NRF_CLOCK->EVENTS_LFCLKSTARTED && guard++ < 200000u) {
    }
    if (!NRF_CLOCK->EVENTS_LFCLKSTARTED) {
        NRF_CLOCK->TASKS_LFCLKSTOP = 1;
        NRF_CLOCK->LFCLKSRC = (CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos);
        NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
        NRF_CLOCK->TASKS_LFCLKSTART = 1;
        guard = 0;
        while (!NRF_CLOCK->EVENTS_LFCLKSTARTED && guard++ < 200000u) {}
    }
}

static uint64_t readTicksRaw() {
    uint32_t ovf1;
    uint32_t ovf2;
    uint32_t cnt;
    do {
        ovf1 = g_rtc2Overflows;
        __DMB();
        cnt = NRF_RTC2->COUNTER;
        __DMB();
        ovf2 = g_rtc2Overflows;
    } while (ovf1 != ovf2);
    return ((uint64_t)ovf2 << 24) | (uint64_t)cnt;
}
#else
static int readLfclkSource() { return -1; }
static void startLfclkIfNeeded() {}
static uint64_t readTicksRaw() {
    return (uint64_t)((uint64_t)millis() * (uint64_t)RTC2_TICKS_PER_SEC / 1000u);
}
#endif

void initDeviceTime() {
    if (g_initialized) {
        return;
    }

#if DEVTIME_HAS_RTC
    startLfclkIfNeeded();

    NRF_RTC2->TASKS_STOP = 1;
    NRF_RTC2->TASKS_CLEAR = 1;
    for (volatile int i = 0; i < 100; ++i) { __NOP(); }

    NRF_RTC2->PRESCALER = RTC2_PRESCALER;
    NRF_RTC2->EVENTS_OVRFLW = 0;
    NRF_RTC2->EVTENSET = RTC_EVTENSET_OVRFLW_Msk;
    NRF_RTC2->INTENSET = RTC_INTENSET_OVRFLW_Msk;

    NVIC_ClearPendingIRQ(RTC2_IRQn);
    NVIC_SetPriority(RTC2_IRQn, 7);
    NVIC_EnableIRQ(RTC2_IRQn);

    NRF_RTC2->TASKS_START = 1;
#endif

    g_baseTicks = 0;
    g_baseEpoch = 0;
    g_bootTicks = 0;
    g_lastSyncTicks = UINT64_MAX;
    g_lastPersistedEpoch = 0;
    g_lastPersistTicks = 0;
    g_status = TIME_UNKNOWN;
    g_rtc2Overflows = 0;

    loadPersistedTime();

    g_initialized = true;
}

void maintainDeviceTime() {
    if (!g_initialized) {
        return;
    }
    uint64_t nowTicks = readTicksRaw();
    if (nowTicks - g_lastPersistTicks >= AUTO_PERSIST_INTERVAL_TICKS) {
        g_lastPersistTicks = nowTicks;
        uint32_t nowEpoch = getDeviceTime();
        if (nowEpoch != 0) {
            (void)savePersistedTime(nowEpoch, false);
        }
    }
}

void setDeviceTime(uint32_t epochSeconds) {
    if (!g_initialized) {
        initDeviceTime();
    }
    if (epochSeconds < MIN_ACCEPTED_EPOCH || epochSeconds > MAX_ACCEPTED_EPOCH) {
        return;
    }

    g_baseTicks = readTicksRaw();
    g_baseEpoch = epochSeconds;
    g_lastSyncTicks = g_baseTicks;
    g_status = TIME_FRESH;

    (void)savePersistedTime(epochSeconds, true);
    g_lastPersistTicks = g_baseTicks;
}

uint32_t getDeviceTime() {
    if (!g_initialized || g_status == TIME_UNKNOWN || g_baseEpoch == 0) {
        return 0;
    }
    uint64_t now = readTicksRaw();
    uint64_t delta = (now >= g_baseTicks) ? (now - g_baseTicks) : 0;
    uint32_t secondsSinceBase = (uint32_t)(delta / RTC2_TICKS_PER_SEC);
    return g_baseEpoch + secondsSinceBase;
}

uint64_t getDeviceTicks() {
    if (!g_initialized) {
        return 0;
    }
    return readTicksRaw();
}

uint32_t ticksToEpoch(uint64_t ticks) {
    if (!g_initialized || g_status == TIME_UNKNOWN || g_baseEpoch == 0) {
        return 0;
    }
    if (ticks >= g_baseTicks) {
        uint64_t delta = ticks - g_baseTicks;
        return g_baseEpoch + (uint32_t)(delta / RTC2_TICKS_PER_SEC);
    }
    uint64_t delta = g_baseTicks - ticks;
    uint32_t seconds = (uint32_t)(delta / RTC2_TICKS_PER_SEC);
    return (g_baseEpoch > seconds) ? (g_baseEpoch - seconds) : 0;
}

bool isDeviceTimeSynced() {
    return g_status != TIME_UNKNOWN;
}

DeviceTimeStatus getDeviceTimeStatus() {
    return g_status;
}

uint32_t getDeviceUptimeSeconds() {
    if (!g_initialized) {
        return 0;
    }
    uint64_t now = readTicksRaw();
    uint64_t delta = (now >= g_bootTicks) ? (now - g_bootTicks) : 0;
    return (uint32_t)(delta / RTC2_TICKS_PER_SEC);
}

uint32_t getSecondsSinceSync() {
    if (!g_initialized || g_lastSyncTicks == UINT64_MAX) {
        return UINT32_MAX;
    }
    uint64_t now = readTicksRaw();
    uint64_t delta = (now >= g_lastSyncTicks) ? (now - g_lastSyncTicks) : 0;
    return (uint32_t)(delta / RTC2_TICKS_PER_SEC);
}

void persistDeviceTime() {
    if (!g_initialized) {
        return;
    }
    uint32_t now = getDeviceTime();
    if (now != 0) {
        (void)savePersistedTime(now, true);
        g_lastPersistTicks = readTicksRaw();
    }
}

void formatEpochUTC(uint32_t epochSeconds, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    if (epochSeconds == 0) {
        snprintf(out, outLen, "----");
        return;
    }
    time_t t = (time_t)epochSeconds;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, outLen, "%Y-%m-%d %H:%M:%S", &tmv);
}

void formatEpochISO(uint32_t epochSeconds, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    if (epochSeconds == 0) {
        if (outLen >= 1) out[0] = '\0';
        return;
    }
    time_t t = (time_t)epochSeconds;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, outLen, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void setDeviceTZOffset(int32_t offsetSeconds) {
    if (offsetSeconds < -14 * 3600 || offsetSeconds > 14 * 3600) {
        return;
    }
    g_tzOffsetSeconds = offsetSeconds;
}

int32_t getDeviceTZOffset() {
    return g_tzOffsetSeconds;
}

void formatEpochLocal(uint32_t epochSeconds, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    if (epochSeconds == 0) {
        snprintf(out, outLen, "----");
        return;
    }
    int32_t tz = g_tzOffsetSeconds;
    time_t t = (time_t)((int64_t)epochSeconds + (int64_t)tz);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char body[20];
    strftime(body, sizeof(body), "%Y-%m-%d %H:%M:%S", &tmv);
    int absOff = tz < 0 ? -tz : tz;
    snprintf(out, outLen, "%s %c%02d:%02d",
             body,
             tz < 0 ? '-' : '+',
             absOff / 3600,
             (absOff % 3600) / 60);
}
