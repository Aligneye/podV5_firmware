#include "storage.h"
#include "session_log.h"
#include "therapy.h"
#include "nrf.h"
#include <RTTStream.h>
#include <string.h>
#include <math.h>

extern RTTStream rtt;

// ── Flash layout ───────────────────────────────────────────────────────────
// nRF52832 has 512 KB flash in 4 KB (0x1000) pages. We reserve the last
// page (0x7F000-0x7FFFF) as a dedicated settings page. Linker scripts for
// this project place the application well below this address.
static constexpr uint32_t SETTINGS_PAGE_ADDR = 0x00073000UL;
static constexpr uint32_t SETTINGS_MAGIC     = 0x414C4733UL;  // "ALG3"
static constexpr uint16_t SETTINGS_VERSION   = 3u;

struct PersistedSettingsV1 {
    uint32_t magic;
    uint16_t version;
    uint8_t  therapySubModeIndex;
    uint8_t  reserved;
};

struct PersistedSettings {
    uint32_t magic;
    uint16_t version;
    uint8_t  trainingDelay;
    uint8_t  reserved;
    float     calY;
    float     calZ;
};

static PersistedSettings g_settings = {
    SETTINGS_MAGIC,
    SETTINGS_VERSION,
    (uint8_t)TRAIN_INSTANT,
    0u,
    6.75f,
    6.75f
};

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
    if (*ver == 1u) {
        g_settings.magic                = SETTINGS_MAGIC;
        g_settings.version              = SETTINGS_VERSION;
        g_settings.trainingDelay        = (uint8_t)TRAIN_INSTANT;
        g_settings.reserved             = 0;
        g_settings.calY                 = 6.75f;
        g_settings.calZ                 = 6.75f;
        persist();  // migrate flash layout v1 -> v3
        return true;
    }
    if (*ver == 2u) {
        const PersistedSettings* v2 =
            reinterpret_cast<const PersistedSettings*>(SETTINGS_PAGE_ADDR);
        g_settings.magic         = SETTINGS_MAGIC;
        g_settings.version       = SETTINGS_VERSION;
        g_settings.trainingDelay = (uint8_t)TRAIN_INSTANT;
        g_settings.reserved      = 0;
        g_settings.calY          = v2->calY;
        g_settings.calZ          = v2->calZ;
        if (fabsf(g_settings.calY) < 0.1f && fabsf(g_settings.calZ) < 0.1f) {
            g_settings.calY = 6.75f;
            g_settings.calZ = 6.75f;
        }
        persist();  // migrate flash layout v2 -> v3
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
