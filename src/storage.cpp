#include "storage.h"
#include "calibration.h"
#include "session_log.h"
#include "therapy.h"
#include "nrf.h"
#include <RTTStream.h>
#include <string.h>
#include <math.h>

extern RTTStream rtt;

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
    {}
};

static constexpr uint8_t ACTIVE_PROFILE_DEFAULT = 0u;
static constexpr uint8_t ACTIVE_PROFILE_MAX_STORED = 8u;
static constexpr uint8_t NEXT_OVERWRITE_DEFAULT = 0u;
static constexpr uint32_t PROFILE_STORE_MAGIC = 0x50524631UL; // "PRF1"
static const char* PROFILE_STORE_PATH = "/profiles.dat";
static const char* PROFILE_STORE_TMP_PATH = "/profiles.tmp";

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

#if PROFILE_STORE_HAS_FS
static bool writeProfileStore() {
    InternalFS.begin();
    InternalFS.remove(PROFILE_STORE_TMP_PATH);

    File tmp = InternalFS.open(PROFILE_STORE_TMP_PATH, FILE_O_WRITE);
    if (!tmp) return false;

    ProfileStore store{};
    store.magic = PROFILE_STORE_MAGIC;
    store.profileCount = g_settings.profileCount;
    store.activeProfileIndex = decodeStoredActiveProfileIndex();
    store.reserved[0] = decodeStoredNextOverwriteIndex();
    memcpy(store.profiles, g_settings.profiles, sizeof(store.profiles));

    const size_t written = tmp.write((uint8_t*)&store, sizeof(store));
    tmp.flush();
    tmp.close();

    if (written != sizeof(store)) {
        InternalFS.remove(PROFILE_STORE_TMP_PATH);
        return false;
    }

    InternalFS.remove(PROFILE_STORE_PATH);
    if (InternalFS.rename(PROFILE_STORE_TMP_PATH, PROFILE_STORE_PATH)) {
        return true;
    }

    File direct = InternalFS.open(PROFILE_STORE_PATH, FILE_O_WRITE);
    if (!direct) {
        InternalFS.remove(PROFILE_STORE_TMP_PATH);
        return false;
    }

    const size_t written2 = direct.write((uint8_t*)&store, sizeof(store));
    direct.flush();
    direct.close();
    InternalFS.remove(PROFILE_STORE_TMP_PATH);
    return written2 == sizeof(store);
}

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
static void persist() {
    // Pad struct to whole 32-bit words for NVMC word writes.
    constexpr uint32_t kWordCount =
        (sizeof(PersistedSettings) + 3u) / 4u;
    uint32_t buffer[kWordCount] = {0};
    memcpy(buffer, &g_settings, sizeof(g_settings));

    noInterrupts();
    nvmcErasePage(SETTINGS_PAGE_ADDR);
    nvmcWriteWords(SETTINGS_PAGE_ADDR, buffer, kWordCount);
    interrupts();
}

static bool loadFromFlash() {
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
        g_settings.profiles[0].createdAt = 0;
        g_settings.profiles[0].sampleCount = 0;
        g_settings.profiles[0].qualityScore = 0;
        g_settings.profiles[0].valid = 1;

        persist();  // migrate flash layout to v4
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
    return true;
}

// ── Public API ─────────────────────────────────────────────────────────────
void storageSetup() {
    if (loadFromFlash()) {
        rtt.print("Storage: loaded, training delay = ");
        rtt.println((int)g_settings.trainingDelay);
    } else {
        rtt.println("Storage: empty, writing defaults");
        persist();
    }
    session_log_init();
}

void saveTrainingDelay(TrainingDelay delay) {
    uint8_t d = (uint8_t)delay;
    if (d < (uint8_t)TRAIN_DELAYED || d > (uint8_t)TRAIN_INSTANT) {
        d = (uint8_t)TRAIN_INSTANT;
    }
    if (g_settings.trainingDelay == d) return;
    g_settings.trainingDelay = d;
    persist();
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

void storageSaveTherapySubMode(uint8_t idx) {
    saveTrainingDelay((TrainingDelay)idx);
    rtt.print("Storage: saved training delay = ");
    rtt.println((int)loadTrainingDelay());
}

bool storageLoadCalibration(float* y, float* z) {
    if (y) *y = g_settings.calY;
    if (z) *z = g_settings.calZ;
    return true;
}

void storageSaveCalibration(float y, float z) {
    if (fabsf(y) < 0.1f && fabsf(z) < 0.1f) {
        y = 6.75f;
        z = 6.75f;
    }
    if (g_settings.calY == y && g_settings.calZ == z) return;
    g_settings.calY = y;
    g_settings.calZ = z;
    persist();
    rtt.println("Storage: saved posture calibration");
}

bool storageLoadProfiles(OrientationProfile* profiles, uint8_t* count) {
    if (!profiles || !count) return false;

#if PROFILE_STORE_HAS_FS
    ProfileStore store{};
    if (readProfileStore(store)) {
        g_settings.profileCount = store.profileCount;
        memcpy(g_settings.profiles, store.profiles, store.profileCount * sizeof(OrientationProfile));
        memset(g_settings.reserved2, 0, sizeof(g_settings.reserved2));
        if (store.activeProfileIndex >= 0 && store.activeProfileIndex < (int8_t)store.profileCount) {
            g_settings.reserved2[0] = (uint8_t)(store.activeProfileIndex + 1);
        }
        g_settings.reserved2[1] = (store.reserved[0] < ACTIVE_PROFILE_MAX_STORED) ? store.reserved[0] : NEXT_OVERWRITE_DEFAULT;

        *count = g_settings.profileCount;
        memcpy(profiles, g_settings.profiles, g_settings.profileCount * sizeof(OrientationProfile));
        return true;
    }
#endif

    *count = g_settings.profileCount;
    if (g_settings.profileCount > 8) {
        g_settings.profileCount = 8;
        *count = 8;
    }
    memcpy(profiles, g_settings.profiles, g_settings.profileCount * sizeof(OrientationProfile));
    return true;
}

void storageSaveProfiles(const OrientationProfile* profiles, uint8_t count) {
    if (!profiles) return;
    uint8_t countToSave = count;
    if (countToSave > 8) countToSave = 8;
    g_settings.profileCount = countToSave;
    memcpy(g_settings.profiles, profiles, countToSave * sizeof(OrientationProfile));

    // For backward compatibility, keep calY and calZ in sync with the first profile
    if (countToSave > 0) {
        g_settings.calY = g_settings.profiles[0].refY;
        g_settings.calZ = g_settings.profiles[0].refZ;
    }

#if PROFILE_STORE_HAS_FS
    if (writeProfileStore()) return;
#endif

    persist();
}

int8_t storageLoadActiveProfileIndex() {
    return decodeStoredActiveProfileIndex();
}

void storageSaveActiveProfileIndex(int8_t index) {
    uint8_t stored = ACTIVE_PROFILE_DEFAULT;
    if (index >= 0 && index < (int8_t)g_settings.profileCount && index < (int8_t)ACTIVE_PROFILE_MAX_STORED) {
        stored = (uint8_t)(index + 1);
    }
    if (g_settings.reserved2[0] == stored) return;

    g_settings.reserved2[0] = stored;
#if PROFILE_STORE_HAS_FS
    if (writeProfileStore()) return;
#endif
    persist();
}

uint8_t storageLoadNextProfileOverwriteIndex() {
    return decodeStoredNextOverwriteIndex();
}

void storageSaveNextProfileOverwriteIndex(uint8_t index) {
    if (index >= ACTIVE_PROFILE_MAX_STORED) index = NEXT_OVERWRITE_DEFAULT;
    if (g_settings.reserved2[1] == index) return;

    g_settings.reserved2[1] = index;
#if PROFILE_STORE_HAS_FS
    if (writeProfileStore()) return;
#endif
    persist();
}

uint32_t storageLoadDefaultProfileId() {
    return decodeStoredDefaultProfileId();
}

void storageSaveDefaultProfileId(uint32_t id) {
    if (g_settings.defaultProfileId == id) return;
    g_settings.defaultProfileId = id;
#if PROFILE_STORE_HAS_FS
    if (writeProfileStore()) return;
#endif
    persist();
}

uint32_t storageLoadNextProfileId() {
    return decodeStoredNextProfileId();
}

void storageSaveNextProfileId(uint32_t id) {
    if (id == 0u) id = 1u;
    if (g_settings.nextProfileId == id) return;
    g_settings.nextProfileId = id;
#if PROFILE_STORE_HAS_FS
    if (writeProfileStore()) return;
#endif
    persist();
}

