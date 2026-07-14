#include "storage.h"
#include "calibration.h"
#include "session_log.h"
#include "therapy.h"
#include "nrf.h"
#include "rtt_debugger.h"
#include <RTTStream.h>
#include <string.h>
#include <math.h>

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
#define PROFILE_STORE_HAS_FS 1
#else
#define PROFILE_STORE_HAS_FS 0
#endif

// ── Flash layout ───────────────────────────────────────────────────────────
// nRF52832 has 512 KB flash in 4 KB (0x1000) pages. We reserve the last
// page (0x7F000-0x7FFFF) as a dedicated settings page. Linker scripts for
// this project place the application well below this address.
static constexpr uint32_t SETTINGS_PAGE_ADDR = 0x00073000UL;
static constexpr uint32_t SETTINGS_MAGIC     = 0x414C4733UL;  // "ALG3"
static constexpr uint16_t SETTINGS_VERSION   = 5u;

struct PersistedSettingsV1 {
    uint32_t magic;
    uint16_t version;
    uint8_t  therapySubModeIndex;
    uint8_t  reserved;
};

struct PersistedSettingsV3 {
    uint32_t magic;
    uint16_t version;
    uint8_t  trainingDelay;
    uint8_t  reserved;
    float     calY;
    float     calZ;
};

struct PersistedSettings {
    uint32_t magic;
    uint16_t version;
    uint8_t  trainingDelay;
    uint8_t  reserved;
    float     calY;
    float     calZ;
    uint8_t            profileCount;
    uint8_t            reserved2[3];
    uint32_t           nextProfileId;
    uint32_t           defaultProfileId;
    OrientationProfile profiles[8];
    uint32_t           crc;
};

static PersistedSettings g_settings = {
    SETTINGS_MAGIC,
    SETTINGS_VERSION,
    (uint8_t)TRAIN_INSTANT,
    0u,
    6.75f,
    6.75f,
    0u,
    {0, 0, 0},
    1u,
    0u,
    {},
    0u
};

static constexpr uint8_t ACTIVE_PROFILE_DEFAULT = 0u;
static constexpr uint8_t ACTIVE_PROFILE_MAX_STORED = 8u;
static constexpr uint8_t NEXT_OVERWRITE_DEFAULT = 0u;
static constexpr uint32_t PROFILE_STORE_MAGIC = 0x50524631UL; // "PRF1"
static const char* PROFILE_STORE_PATH = "/profiles.dat";
static const char* PROFILE_STORE_TMP_PATH = "/profiles.tmp";
static const char* SETTINGS_FILE_PATH = "/sys_settings.dat";
static const char* SETTINGS_TMP_PATH  = "/sys_settings.tmp";
static bool s_storageInitialized = false;

struct ProfileStore {
    uint32_t magic;
    uint8_t  profileCount;
    int8_t   activeProfileIndex;
    uint8_t  reserved[2];
    OrientationProfile profiles[8];
};

static int8_t decodeStoredActiveProfileIndex() {
    const uint8_t stored = g_settings.reserved2[0];
    if (stored == ACTIVE_PROFILE_DEFAULT) return -1;
    if (stored > ACTIVE_PROFILE_MAX_STORED) return -1;

    const int8_t index = (int8_t)(stored - 1u);
    if (index >= (int8_t)g_settings.profileCount) return -1;
    return index;
}

static uint8_t decodeStoredNextOverwriteIndex() {
    const uint8_t stored = g_settings.reserved2[1];
    if (stored >= ACTIVE_PROFILE_MAX_STORED) return NEXT_OVERWRITE_DEFAULT;
    return stored;
}

static uint32_t decodeStoredNextProfileId() {
    if (g_settings.nextProfileId == 0u) return 1u;
    return g_settings.nextProfileId;
}

static uint32_t decodeStoredDefaultProfileId() {
    return g_settings.defaultProfileId;
}

static uint32_t computeSettingsCrc(const PersistedSettings& settings) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&settings);
    uint32_t crc = 2166136261u;
    for (size_t i = 0; i < sizeof(PersistedSettings) - sizeof(uint32_t); i++) {
        crc ^= bytes[i];
        crc *= 16777619u;
    }
    return crc;
}

#if PROFILE_STORE_HAS_FS
// /profiles.dat is legacy: everything it held is a subset of the settings
// file, which is now the single source of truth. The file is only read once
// at boot to migrate devices upgrading from firmware that still wrote it
// (where it could be newer than the settings file), then deleted.
static bool readProfileStore(ProfileStore& store) {
    InternalFS.begin();
    File file = InternalFS.open(PROFILE_STORE_PATH, FILE_O_READ);
    if (!file) return false;

    const int readBytes = file.read((uint8_t*)&store, sizeof(store));
    file.close();

    if (readBytes != (int)sizeof(store)) return false;
    if (store.magic != PROFILE_STORE_MAGIC) return false;
    if (store.profileCount > 8) store.profileCount = 8;
    return true;
}
#endif

// ── NVMC helpers ───────────────────────────────────────────────────────────
static inline void nvmcWaitReady() {
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) { /* spin */ }
}

static void nvmcErasePage(uint32_t addr) {
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
    nvmcWaitReady();
    NRF_NVMC->ERASEPAGE = addr;
    nvmcWaitReady();
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    nvmcWaitReady();
}

static void nvmcWriteWords(uint32_t addr, const uint32_t* src, uint32_t count) {
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    nvmcWaitReady();
    volatile uint32_t* dst = reinterpret_cast<volatile uint32_t*>(addr);
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = src[i];
        nvmcWaitReady();
    }
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    nvmcWaitReady();
}

// ── Persist / load ─────────────────────────────────────────────────────────
static bool persist() {
#if PROFILE_STORE_HAS_FS
    const uint32_t t0 = millis();
    InternalFS.begin();
    File file = InternalFS.open(SETTINGS_FILE_PATH, FILE_O_WRITE);
    if (file) {
        g_settings.crc = computeSettingsCrc(g_settings);
        file.seek(0);  // FILE_O_WRITE opens positioned at end of file
        size_t written = file.write(reinterpret_cast<const uint8_t*>(&g_settings), sizeof(g_settings));
        file.truncate(sizeof(g_settings));
        file.close();
        s_rttTiming.print("[TIMING] settings persist: ");
        s_rttTiming.print(millis() - t0);
        s_rttTiming.println(" ms");
        if (written == sizeof(g_settings)) {
            rtt.println("[STORAGE] Settings persisted to FS successfully");
            return true;
        }
    }
    rtt.println("[STORAGE] WARNING: FS persist failed");
#endif

    // Only allow NVMC fallback at boot time before storage setup completes.
    // At runtime (BLE active, calibration running), direct NVMC flash writes are blocked to avoid CPU reset.
    if (s_storageInitialized) {
        rtt.println("[STORAGE] ERROR: FS persist failed; NVMC fallback blocked during active runtime");
        return false;
    }

    // Pad struct to whole 32-bit words for NVMC word writes.
    constexpr uint32_t kWordCount =
        (sizeof(PersistedSettings) + 3u) / 4u;
    uint32_t buffer[kWordCount] = {0};
    g_settings.crc = computeSettingsCrc(g_settings);
    memcpy(buffer, &g_settings, sizeof(g_settings));

    noInterrupts();
    nvmcErasePage(SETTINGS_PAGE_ADDR);
    nvmcWriteWords(SETTINGS_PAGE_ADDR, buffer, kWordCount);
    interrupts();
    rtt.println("[STORAGE] Settings persisted to NVMC (boot-time)");
    return true;
}

static bool loadFromFlash() {
#if PROFILE_STORE_HAS_FS
    InternalFS.begin();
    File file = InternalFS.open(SETTINGS_FILE_PATH, FILE_O_READ);
    if (file) {
        PersistedSettings tempSettings;
        int readBytes = file.read(reinterpret_cast<uint8_t*>(&tempSettings), sizeof(tempSettings));
        file.close();
        if (readBytes == (int)sizeof(tempSettings) && tempSettings.magic == SETTINGS_MAGIC) {
            uint32_t expectedCrc = computeSettingsCrc(tempSettings);
            if (tempSettings.crc == 0u || tempSettings.crc == expectedCrc) {
                g_settings = tempSettings;
                rtt.println("[STORAGE] Settings loaded from FS successfully");
                return true;
            }
        }
        rtt.println("[STORAGE] Invalid settings file on FS, falling back to NVMC");
    }
#endif

    const uint8_t* base = reinterpret_cast<const uint8_t*>(SETTINGS_PAGE_ADDR);
    const uint32_t* magic = reinterpret_cast<const uint32_t*>(base);
    if (*magic != SETTINGS_MAGIC) return false;

    const uint16_t* ver = reinterpret_cast<const uint16_t*>(base + 4);
    if (*ver == 1u || *ver == 2u || *ver == 3u) {
        uint8_t trainingDelay = (uint8_t)TRAIN_INSTANT;
        float calY = 6.75f;
        float calZ = 6.75f;

        if (*ver == 2u || *ver == 3u) {
            const PersistedSettingsV3* oldSettings =
                reinterpret_cast<const PersistedSettingsV3*>(SETTINGS_PAGE_ADDR);
            trainingDelay = oldSettings->trainingDelay;
            calY = oldSettings->calY;
            calZ = oldSettings->calZ;
        }

        if (fabsf(calY) < 0.1f && fabsf(calZ) < 0.1f) {
            calY = 6.75f;
            calZ = 6.75f;
        }

        // Reconstruct refX using the heuristic check for gravity units (calY^2 + calZ^2 < 2.0f)
        float magSq = calY * calY + calZ * calZ;
        float refX = 0.0f;
        if (magSq < 2.0f) {
            // G-units
            float diff = 1.0f - magSq;
            refX = (diff > 0.0f) ? sqrtf(diff) : 0.0f;
        } else {
            // m/s^2
            float diff = (9.80665f * 9.80665f) - magSq;
            refX = (diff > 0.0f) ? sqrtf(diff) : 0.0f;
        }

        g_settings.magic = SETTINGS_MAGIC;
        g_settings.version = SETTINGS_VERSION;
        g_settings.trainingDelay = trainingDelay;
        g_settings.reserved = 0;
        g_settings.calY = calY;
        g_settings.calZ = calZ;
        g_settings.profileCount = 1;
        memset(g_settings.reserved2, 0, sizeof(g_settings.reserved2));
        g_settings.nextProfileId = 2u;
        g_settings.defaultProfileId = 0u;

        g_settings.profiles[0].id = 1u;
        strncpy(g_settings.profiles[0].name, "Default Vertical", sizeof(g_settings.profiles[0].name) - 1);
        g_settings.profiles[0].name[sizeof(g_settings.profiles[0].name) - 1] = '\0';
        g_settings.profiles[0].refX = refX;
        g_settings.profiles[0].refY = calY;
        g_settings.profiles[0].refZ = calZ;
        g_settings.profiles[0].createdAtEpoch = 0;
        g_settings.profiles[0].sampleCount = 0;
        g_settings.profiles[0].stabilityScore = 0.0f;
        g_settings.profiles[0].valid = 1;

        g_settings.crc = 0u;
        persist();  // migrate flash layout to v4
        rtt.println("[STORAGE] Settings migrated from legacy NVMC (v1-v3)");
        return true;
    }
    if (*ver != SETTINGS_VERSION) return false;

    const PersistedSettings* flash =
        reinterpret_cast<const PersistedSettings*>(SETTINGS_PAGE_ADDR);
    if (flash->trainingDelay < (uint8_t)TRAIN_DELAYED ||
        flash->trainingDelay > (uint8_t)TRAIN_INSTANT) {
        return false;
    }

    g_settings = *flash;
    if (g_settings.nextProfileId == 0u) g_settings.nextProfileId = 1u;
    if (fabsf(g_settings.calY) < 0.1f && fabsf(g_settings.calZ) < 0.1f) {
        g_settings.calY = 6.75f;
        g_settings.calZ = 6.75f;
    }
    const uint32_t expectedCrc = computeSettingsCrc(g_settings);
    if (g_settings.crc != 0u && g_settings.crc != expectedCrc) {
        return false;
    }

    // We loaded it from NVMC successfully, now save it to FS for future accesses.
    rtt.println("[STORAGE] Settings loaded from NVMC, saving to FS");
    persist();
    return true;
}

// ── Write batching & deferred persist ──────────────────────────────────────
// A batch defers the flash write so a multi-step update (e.g. saving a new
// calibration profile + active index + default id) costs one settings commit
// instead of one per setter. Setters still update g_settings immediately;
// only the flash write is deferred until the batch is committed.
//
// A deferred persist pushes the commit out of the calling code path entirely:
// it runs from storageLoop() in the main loop, so time-critical feedback
// (BLE notify, success haptic) is never blocked behind the flash write.
// Because persist() always writes the full settings struct, any later
// successful persist heals an earlier failed or pending one.
static uint8_t s_batchDepth = 0;
static bool s_batchSettingsDirty = false;
static bool s_deferredPersistPending = false;
static uint32_t s_deferredPersistDueMs = 0;
static uint8_t s_deferredPersistRetries = 0;

static bool persistSettings() {
    if (s_batchDepth > 0) {
        s_batchSettingsDirty = true;
        return true;
    }
    return persist();
}

void storageBeginBatch() {
    s_batchDepth++;
}

bool storageCommitBatch() {
    if (s_batchDepth == 0) return true;
    if (--s_batchDepth > 0) return true;
    if (!s_batchSettingsDirty) return true;
    s_batchSettingsDirty = false;
    return persist();
}

void storageCommitBatchDeferred() {
    if (s_batchDepth == 0) return;
    if (--s_batchDepth > 0) return;
    if (!s_batchSettingsDirty) return;
    s_batchSettingsDirty = false;
    s_deferredPersistPending = true;
    // Small grace period so the immediate post-save BLE traffic (result
    // notify, app's GET_PROFILES round-trip) isn't competing with flash ops
    // for radio-free slots.
    s_deferredPersistDueMs = millis() + 150u;
    s_deferredPersistRetries = 0;
}

void storageLoop() {
    if (!s_deferredPersistPending) return;
    if ((int32_t)(millis() - s_deferredPersistDueMs) < 0) return;
    if (persist()) {
        s_deferredPersistPending = false;
        return;
    }
    if (++s_deferredPersistRetries >= 5u) {
        s_deferredPersistPending = false;
        s_rttTiming.println("[STORAGE] ERROR: deferred persist gave up after retries");
        return;
    }
    s_deferredPersistDueMs = millis() + 1000u;
}

// ── Public API ─────────────────────────────────────────────────────────────
void storageSetup() {
    if (loadFromFlash()) {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"t\":\"STORAGE\",\"event\":\"loaded\",\"training_delay\":%u}", (unsigned)g_settings.trainingDelay);
        logPacket("EVT", buf);
    } else {
        logEvent("STORAGE", "empty_defaults");
        persist();
    }
#if PROFILE_STORE_HAS_FS
    // One-time migration from older firmware: /profiles.dat could be newer
    // than the settings file (old code skipped the settings persist when the
    // profile store write succeeded), so merge it in, persist once, then
    // delete it. Afterwards the settings file is the single source of truth.
    ProfileStore store{};
    if (readProfileStore(store)) {
        g_settings.profileCount = store.profileCount;
        memcpy(g_settings.profiles, store.profiles, store.profileCount * sizeof(OrientationProfile));
        memset(g_settings.reserved2, 0, sizeof(g_settings.reserved2));
        if (store.activeProfileIndex >= 0 && store.activeProfileIndex < (int8_t)store.profileCount) {
            g_settings.reserved2[0] = (uint8_t)(store.activeProfileIndex + 1);
        }
        g_settings.reserved2[1] = (store.reserved[0] < ACTIVE_PROFILE_MAX_STORED) ? store.reserved[0] : NEXT_OVERWRITE_DEFAULT;
        if (persist()) {
            InternalFS.remove(PROFILE_STORE_PATH);
            logEvent("STORAGE", "profile_store_migrated");
        }
    }
    // One-time cleanup of temp files left by older firmware's temp+rename
    // persist pattern (no longer used).
    InternalFS.remove(PROFILE_STORE_TMP_PATH);
    InternalFS.remove(SETTINGS_TMP_PATH);
#endif
    s_storageInitialized = true;
    session_log_init();
}

bool saveTrainingDelay(TrainingDelay delay) {
    uint8_t d = (uint8_t)delay;
    if (d < (uint8_t)TRAIN_DELAYED || d > (uint8_t)TRAIN_INSTANT) {
        d = (uint8_t)TRAIN_INSTANT;
    }
    if (g_settings.trainingDelay == d) return true;
    g_settings.trainingDelay = d;
    return persistSettings();
}

TrainingDelay loadTrainingDelay() {
    uint8_t d = g_settings.trainingDelay;
    if (d < (uint8_t)TRAIN_DELAYED || d > (uint8_t)TRAIN_INSTANT) {
        d = (uint8_t)TRAIN_INSTANT;
    }
    return (TrainingDelay)d;
}

uint8_t storageLoadTherapySubMode() {
    return (uint8_t)loadTrainingDelay();
}

bool storageSaveTherapySubMode(uint8_t idx) {
    bool ok = saveTrainingDelay((TrainingDelay)idx);
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"t\":\"STORAGE\",\"event\":\"save_training_delay\",\"value\":%u}", (unsigned)loadTrainingDelay());
    logPacket("EVT", buf);
    return ok;
}

bool storageLoadCalibration(float* y, float* z) {
    if (y) *y = g_settings.calY;
    if (z) *z = g_settings.calZ;
    return true;
}

bool storageSaveCalibration(float y, float z) {
    if (fabsf(y) < 0.1f && fabsf(z) < 0.1f) {
        y = 6.75f;
        z = 6.75f;
    }
    if (g_settings.calY == y && g_settings.calZ == z) return true;
    g_settings.calY = y;
    g_settings.calZ = z;
    bool ok = persistSettings();
    logEvent("STORAGE", "saved_posture_calibration");
    return ok;
}

void storageFactoryReset() {
    g_settings.trainingDelay = (uint8_t)TRAIN_INSTANT;
    g_settings.calY = 6.75f;
    g_settings.calZ = 6.75f;
    g_settings.profileCount = 0;
    memset(g_settings.reserved2, 0, sizeof(g_settings.reserved2));
    g_settings.nextProfileId = 1u;
    g_settings.defaultProfileId = 0u;
    memset(g_settings.profiles, 0, sizeof(g_settings.profiles));
    persistSettings();
    logEvent("STORAGE", "factory_reset");
}

bool storageLoadProfiles(OrientationProfile* profiles, uint8_t* count) {
    if (!profiles || !count) return false;

    // Profiles live in the settings struct; any legacy /profiles.dat was
    // already merged into g_settings during storageSetup().
    *count = g_settings.profileCount;
    if (g_settings.profileCount > 8) {
        g_settings.profileCount = 8;
        *count = 8;
    }
    if (g_settings.profileCount == 0) {
        return true;
    }
    memcpy(profiles, g_settings.profiles, g_settings.profileCount * sizeof(OrientationProfile));
    return true;
}

bool storageSaveProfiles(const OrientationProfile* profiles, uint8_t count) {
    if (!profiles) return false;
    uint8_t countToSave = count;
    if (countToSave > 8) countToSave = 8;
    g_settings.profileCount = countToSave;
    memcpy(g_settings.profiles, profiles, countToSave * sizeof(OrientationProfile));

    // For backward compatibility, keep calY and calZ in sync with the first profile
    if (countToSave > 0) {
        g_settings.calY = g_settings.profiles[0].refY;
        g_settings.calZ = g_settings.profiles[0].refZ;
    }

    return persistSettings();
}

int8_t storageLoadActiveProfileIndex() {
    return decodeStoredActiveProfileIndex();
}

bool storageSaveActiveProfileIndex(int8_t index) {
    uint8_t stored = ACTIVE_PROFILE_DEFAULT;
    if (index >= 0 && index < (int8_t)g_settings.profileCount && index < (int8_t)ACTIVE_PROFILE_MAX_STORED) {
        stored = (uint8_t)(index + 1);
    }
    if (g_settings.reserved2[0] == stored) return true;

    g_settings.reserved2[0] = stored;
    return persistSettings();
}

uint8_t storageLoadNextProfileOverwriteIndex() {
    return decodeStoredNextOverwriteIndex();
}

bool storageSaveNextProfileOverwriteIndex(uint8_t index) {
    if (index >= ACTIVE_PROFILE_MAX_STORED) index = NEXT_OVERWRITE_DEFAULT;
    if (g_settings.reserved2[1] == index) return true;

    g_settings.reserved2[1] = index;
    return persistSettings();
}

uint32_t storageLoadDefaultProfileId() {
    return decodeStoredDefaultProfileId();
}

bool storageSaveDefaultProfileId(uint32_t id) {
    if (g_settings.defaultProfileId == id) return true;
    g_settings.defaultProfileId = id;
    return persistSettings();
}

uint32_t storageLoadNextProfileId() {
    return decodeStoredNextProfileId();
}

bool storageSaveNextProfileId(uint32_t id) {
    if (id == 0u) id = 1u;
    if (g_settings.nextProfileId == id) return true;
    g_settings.nextProfileId = id;
    return persistSettings();
}

