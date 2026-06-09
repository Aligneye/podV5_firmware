#include "calibration.h"
#include "storage.h"
#include "training.h"
#include <math.h>
#include <string.h>
#include <RTTStream.h>

extern RTTStream rtt;

// ── Profile Database ────────────────────────────────────────────────────────
static OrientationProfile s_profiles[8];
static uint8_t s_profileCount = 0;
static int s_activeProfileIndex = -1;

constexpr float PROFILE_MATCH_THRESHOLD = 0.95f;  // Strict check (~18 deg max tilt)
constexpr float PROFILE_AMBIGUITY_MARGIN = 0.05f; // Margin to prevent close matching

// ── Helpers ─────────────────────────────────────────────────────────────────
static inline void copyProfileName(char* dst, const char* src) {
    strncpy(dst, src, 15);
    dst[15] = '\0';
}

static inline float magnitude3D(float x, float y, float z) {
    return sqrtf(x*x + y*y + z*z);
}

static inline bool normalize3D(float x, float y, float z, float& nx, float& ny, float& nz) {
    float mag = magnitude3D(x, y, z);
    if (mag < 0.001f) {
        nx = 0.0f; ny = 0.0f; nz = 0.0f;
        return false;
    }
    nx = x / mag;
    ny = y / mag;
    nz = z / mag;
    return true;
}

// ── Profile APIs ────────────────────────────────────────────────────────────
void initProfiles() {
    memset(s_profiles, 0, sizeof(s_profiles));
    s_profileCount = 0;
    s_activeProfileIndex = -1;

    if (!storageLoadProfiles(s_profiles, &s_profileCount)) {
        s_profileCount = 0;
        s_activeProfileIndex = -1;
    }
}

bool addCalibrationProfile(const char* name) {
    if (!name || strlen(name) == 0) return false;
    if (!isLastCalibrationValid()) return false;
    if (s_profileCount >= 8) return false;

    // Check if name already exists (if so, overwrite)
    int targetIndex = s_profileCount;
    for (uint8_t i = 0; i < s_profileCount; i++) {
        if (strcmp(s_profiles[i].name, name) == 0) {
            targetIndex = i;
            break;
        }
    }

    OrientationProfile& p = s_profiles[targetIndex];
    copyProfileName(p.name, name);
    p.refX = getLastCalibratedX();
    p.refY = getLastCalibratedY();
    p.refZ = getLastCalibratedZ();
    p.createdAt = millis();

    if (targetIndex == s_profileCount) {
        s_profileCount++;
    }

    storageSaveProfiles(s_profiles, s_profileCount);
    rtt.printf("PROFILE CREATED: %s\n", p.name);

    detectCurrentOrientationProfile();
    return true;
}

bool deleteCalibrationProfile(uint8_t index) {
    if (index >= s_profileCount) return false;

    for (uint8_t i = index; i < s_profileCount - 1; i++) {
        s_profiles[i] = s_profiles[i + 1];
    }
    s_profileCount--;
    storageSaveProfiles(s_profiles, s_profileCount);

    detectCurrentOrientationProfile();
    return true;
}

uint8_t getProfileCount() {
    return s_profileCount;
}

const OrientationProfile* getProfile(uint8_t index) {
    if (index < s_profileCount) {
        return &s_profiles[index];
    }
    return nullptr;
}

int getActiveProfileIndex() {
    return s_activeProfileIndex;
}

const OrientationProfile* getActiveProfile() {
    if (s_activeProfileIndex >= 0 && s_activeProfileIndex < s_profileCount) {
        return &s_profiles[s_activeProfileIndex];
    }
    return nullptr;
}

bool detectCurrentOrientationProfile() {
    int prevIdx = s_activeProfileIndex;

    if (rawX == 0.0f && rawY == 0.0f && rawZ == 0.0f) {
        s_activeProfileIndex = -1;
        if (prevIdx != -1) {
            rtt.println("ORIENTATION CHANGED: UNKNOWN (no raw sensor data)");
        }
        rtt.println("PROFILE NOT FOUND");
        return false;
    }

    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    if (!normalize3D(rawX, rawY, rawZ, cx, cy, cz)) {
        s_activeProfileIndex = -1;
        if (prevIdx != -1) {
            rtt.println("ORIENTATION CHANGED: UNKNOWN (normalization failed)");
        }
        rtt.println("PROFILE NOT FOUND");
        return false;
    }

    int bestIdx = -1;
    float bestScore = -1.0f;
    float secondScore = -1.0f;

    for (uint8_t i = 0; i < s_profileCount; i++) {
        const OrientationProfile& p = s_profiles[i];
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        if (!normalize3D(p.refX, p.refY, p.refZ, rx, ry, rz)) {
            continue;
        }

        // Cosine similarity
        float score = cx*rx + cy*ry + cz*rz;
        if (score > bestScore) {
            secondScore = bestScore;
            bestScore = score;
            bestIdx = i;
        } else if (score > secondScore) {
            secondScore = score;
        }
    }

    if (bestIdx >= 0 && bestScore >= PROFILE_MATCH_THRESHOLD) {
        if (s_profileCount > 1 && (bestScore - secondScore) <= PROFILE_AMBIGUITY_MARGIN) {
            s_activeProfileIndex = -1;
            if (prevIdx != -1) {
                rtt.println("ORIENTATION CHANGED: UNKNOWN (ambiguity limit)");
            }
            rtt.println("PROFILE NOT FOUND");
            return false;
        }

        s_activeProfileIndex = bestIdx;
        if (prevIdx != s_activeProfileIndex) {
            rtt.printf("ORIENTATION CHANGED: %s (refX: %s, refY: %s, refZ: %s)\n",
                       s_profiles[bestIdx].name,
                       String(s_profiles[bestIdx].refX, 2).c_str(),
                       String(s_profiles[bestIdx].refY, 2).c_str(),
                       String(s_profiles[bestIdx].refZ, 2).c_str());
        }
        rtt.println("PROFILE MATCHED");
        setPostureOrigin3D(s_profiles[bestIdx].refX, s_profiles[bestIdx].refY, s_profiles[bestIdx].refZ);
        return true;
    }

    s_activeProfileIndex = -1;
    if (prevIdx != -1) {
        rtt.println("ORIENTATION CHANGED: UNKNOWN (no matching profile)");
    }
    rtt.println("PROFILE NOT FOUND");
    return false;
}

void updateActiveProfileReference(float refX, float refY, float refZ) {
    if (s_activeProfileIndex >= 0 && s_activeProfileIndex < s_profileCount) {
        s_profiles[s_activeProfileIndex].refX = refX;
        s_profiles[s_activeProfileIndex].refY = refY;
        s_profiles[s_activeProfileIndex].refZ = refZ;
        storageSaveProfiles(s_profiles, s_profileCount);
    }
}

void addOrUpdateProfile0(float refX, float refY, float refZ) {
    if (s_profileCount == 0) {
        s_profileCount = 1;
        copyProfileName(s_profiles[0].name, "Default Vertical");
        s_profiles[0].refX = refX;
        s_profiles[0].refY = refY;
        s_profiles[0].refZ = refZ;
        s_profiles[0].createdAt = millis();
        storageSaveProfiles(s_profiles, s_profileCount);
        s_activeProfileIndex = 0;
        rtt.println("PROFILE CREATED: Default Vertical");
    } else {
        s_profiles[0].refX = refX;
        s_profiles[0].refY = refY;
        s_profiles[0].refZ = refZ;
        storageSaveProfiles(s_profiles, s_profileCount);
    }
}
