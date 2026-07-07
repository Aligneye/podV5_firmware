#include "calibration.h"
#include "storage.h"
#include "training.h"
#include <RTTStream.h>
#include <math.h>
#include <string.h>

extern RTTStream rtt;

static OrientationProfile s_profiles[8];
static uint8_t s_profileCount = 0;
static uint32_t s_activeProfileId = 0;
static uint32_t s_defaultProfileId = 0;

static inline void copyProfileName(char* dst, const char* src) {
    strncpy(dst, src, 23);
    dst[23] = '\0';
}

static void assignProfileDefaults(OrientationProfile& p) {
    memset(&p, 0, sizeof(p));
    p.valid = 1;
}

static int findProfileIndexByIdInternal(uint32_t id) {
    for (uint8_t i = 0; i < s_profileCount; i++) {
        if (s_profiles[i].valid && s_profiles[i].id == id) return (int)i;
    }
    return -1;
}

static uint32_t nextProfileId() {
    uint32_t id = storageLoadNextProfileId();
    if (id == 0u) id = 1u;
    storageSaveNextProfileId(id + 1u);
    return id;
}

uint8_t getProfileCount() { return s_profileCount; }
const OrientationProfile* getProfile(uint8_t index) { return (index < s_profileCount) ? &s_profiles[index] : nullptr; }
const OrientationProfile* getProfileById(uint32_t id) { int idx = findProfileIndexByIdInternal(id); return idx >= 0 ? &s_profiles[idx] : nullptr; }
int getProfileIndexById(uint32_t id) { return findProfileIndexByIdInternal(id); }
uint32_t getProfileIdByIndex(uint8_t index) { return (index < s_profileCount) ? s_profiles[index].id : 0u; }
int getActiveProfileIndex() { return (int)findProfileIndexByIdInternal(s_activeProfileId); }
const OrientationProfile* getActiveProfile() { return getProfileById(s_activeProfileId); }
uint32_t getDefaultProfileId() { return s_defaultProfileId; }

static void setOrientationLabel(const char* label) {
    strncpy(orientationText, label, sizeof(orientationText) - 1);
    orientationText[sizeof(orientationText) - 1] = '\0';
}

void initProfiles() {
    memset(s_profiles, 0, sizeof(s_profiles));
    s_profileCount = 0;
    s_activeProfileId = 0;
    s_defaultProfileId = storageLoadDefaultProfileId();

    if (!storageLoadProfiles(s_profiles, &s_profileCount)) {
        s_profileCount = 0;
    }

    uint32_t maxId = 0;
    for (uint8_t i = 0; i < s_profileCount; i++) {
        if (s_profiles[i].id > maxId) {
            maxId = s_profiles[i].id;
        }
    }

    uint32_t nextId = storageLoadNextProfileId();
    if (nextId <= maxId) {
        nextId = maxId + 1;
    }

    bool updated = false;
    for (uint8_t i = 0; i < s_profileCount; i++) {
        if (s_profiles[i].id == 0u) {
            s_profiles[i].id = nextId++;
            updated = true;
        }
        for (uint8_t j = 0; j < i; j++) {
            if (s_profiles[i].id == s_profiles[j].id) {
                s_profiles[i].id = nextId++;
                updated = true;
                break;
            }
        }
        s_profiles[i].valid = 1;
    }

    if (updated) {
        storageSaveNextProfileId(nextId);
        storageSaveProfiles(s_profiles, s_profileCount);
    }

    if (storageLoadActiveProfileIndex() >= 0 && storageLoadActiveProfileIndex() < (int8_t)s_profileCount) {
        s_activeProfileId = s_profiles[storageLoadActiveProfileIndex()].id;
    }
    // Do not force defaultProfileId to s_profiles[0].id if it is 0,
    // as 0 represents "System default" which is a valid default choice.
    const OrientationProfile* active = getActiveProfile();
    if (active) {
        setPostureOrigin3D(active->refX, active->refY, active->refZ);
        setOrientationLabel(active->name);
    } else {
        selectDefaultCalibrationProfile();
    }
}

bool addCalibrationProfile(const char* name) {
    if (!name || !*name || !isLastCalibrationValid()) return false;
    if (s_profileCount >= 8) return false;

    OrientationProfile& p = s_profiles[s_profileCount++];
    const uint8_t newIndex = (uint8_t)(s_profileCount - 1u);
    assignProfileDefaults(p);
    p.id = nextProfileId();
    copyProfileName(p.name, name);
    p.refX = getLastCalibratedX();
    p.refY = getLastCalibratedY();
    p.refZ = getLastCalibratedZ();
    p.createdAtEpoch = millis() / 1000UL;
    p.sampleCount = 0;
    p.stabilityScore = 0.0f;
    if (!storageSaveProfiles(s_profiles, s_profileCount)) {
        s_profileCount--; // Rollback in-memory profile count
        return false;
    }

    // Activate the newly created profile immediately so the running device uses it.
    // The storage layer persists active profiles by index, not by profile id.
    s_activeProfileId = p.id;
    storageSaveActiveProfileIndex((int8_t)newIndex);
    setPostureOrigin3D(p.refX, p.refY, p.refZ);
    setOrientationLabel(p.name);

    if (s_defaultProfileId == 0u) {
        s_defaultProfileId = p.id;
        storageSaveDefaultProfileId(s_defaultProfileId);
    }
    return true;
}

bool addNextCalibrationProfile() {
    char name[16];
    snprintf(name, sizeof(name), "Profile %u", (unsigned)(s_profileCount + 1u));
    return addCalibrationProfile(name);
}

bool deleteCalibrationProfileById(uint32_t id) {
    if (s_defaultProfileId == id) return false; // Cannot delete the default profile
    int index = findProfileIndexByIdInternal(id);
    if (index < 0) return false;

    // Backup current state for rollback
    OrientationProfile backup[8];
    memcpy(backup, s_profiles, sizeof(s_profiles));
    uint8_t backupCount = s_profileCount;

    for (uint8_t i = (uint8_t)index; i + 1 < s_profileCount; i++) {
        s_profiles[i] = s_profiles[i + 1];
    }
    s_profileCount--;
    if (!storageSaveProfiles(s_profiles, s_profileCount)) {
        // Rollback on write failure
        memcpy(s_profiles, backup, sizeof(s_profiles));
        s_profileCount = backupCount;
        return false;
    }
    if (s_activeProfileId == id) selectDefaultCalibrationProfile();
    if (s_defaultProfileId == id) {
        s_defaultProfileId = (s_profileCount > 0) ? s_profiles[0].id : 0u;
        storageSaveDefaultProfileId(s_defaultProfileId);
    }
    return true;
}

bool renameCalibrationProfileById(uint32_t id, const char* name) {
    int index = findProfileIndexByIdInternal(id);
    if (index < 0 || !name || !*name) return false;
    char oldName[24];
    strncpy(oldName, s_profiles[index].name, sizeof(oldName));
    copyProfileName(s_profiles[index].name, name);
    if (!storageSaveProfiles(s_profiles, s_profileCount)) {
        // Rollback rename on write failure
        strncpy(s_profiles[index].name, oldName, sizeof(s_profiles[index].name));
        return false;
    }
    return true;
}

void clearCalibrationProfiles() {
    memset(s_profiles, 0, sizeof(s_profiles));
    s_profileCount = 0;
    s_activeProfileId = 0;
    s_defaultProfileId = 0;
    storageSaveProfiles(s_profiles, s_profileCount);
    storageSaveActiveProfileIndex(-1);
    storageSaveDefaultProfileId(0u);
    setPostureOrigin(6.75f, 6.75f);
    setOrientationLabel("DEFAULT");
}

bool selectCalibrationProfile(uint8_t index) {
    if (index >= s_profileCount) return false;
    selectCalibrationProfileById(s_profiles[index].id);
    return true;
}

bool selectCalibrationProfileById(uint32_t id) {
    if (id == 0u) {
        selectDefaultCalibrationProfile();
        return true;
    }
    int index = findProfileIndexByIdInternal(id);
    if (index < 0) return false;
    s_activeProfileId = id;
    storageSaveActiveProfileIndex(index);
    setPostureOrigin3D(s_profiles[index].refX, s_profiles[index].refY, s_profiles[index].refZ);
    setOrientationLabel(s_profiles[index].name);
    return true;
}

void setProfileDefaultById(uint32_t id) {
    if (id == 0u || findProfileIndexByIdInternal(id) >= 0) {
        s_defaultProfileId = id;
        storageSaveDefaultProfileId(id);
    }
}

void selectDefaultCalibrationProfile() {
    s_activeProfileId = 0u;
    storageSaveActiveProfileIndex(-1);
    setPostureOrigin(6.75f, 6.75f);
    setOrientationLabel("DEFAULT");
}

bool detectCurrentOrientationProfile() {
    return false;
}

void updateActiveProfileReference(float refX, float refY, float refZ) {
    int index = getActiveProfileIndex();
    if (index >= 0) {
        s_profiles[index].refX = refX;
        s_profiles[index].refY = refY;
        s_profiles[index].refZ = refZ;
        storageSaveProfiles(s_profiles, s_profileCount);
    }
}

void addOrUpdateProfile0(float refX, float refY, float refZ) {
    if (s_profileCount == 0) {
        OrientationProfile& p = s_profiles[0];
        assignProfileDefaults(p);
        p.id = nextProfileId();
        copyProfileName(p.name, "Default Vertical");
        p.refX = refX;
        p.refY = refY;
        p.refZ = refZ;
        p.createdAtEpoch = millis() / 1000UL;
        p.valid = 1;
        s_profileCount = 1;
        s_defaultProfileId = p.id;
        storageSaveDefaultProfileId(p.id);
        storageSaveProfiles(s_profiles, s_profileCount);
        selectCalibrationProfileById(p.id);
    }
}
