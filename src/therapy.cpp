#include "therapy.h"
#include "button.h"
#include "calibration.h"
#include "motor.h"
#include "session_stats.h"
#include <math.h>

extern RTTStream rtt;

static const char* PATTERN_NAMES[] = {
    "Muscle Act",  "Rev Ramp",     "Ramp",       "Wave",         "Slow Wave",
    "Sine Wave",   "Triangle",     "Dbl Wave",   "Anti-Fatigue", "Pulse Ramp",
    "Triple Base", "Const Triple", "Exp Double", "Breath ExpSq"
};

static TherapyState therapyState = THERAPY_IDLE;
static unsigned long therapyStartMs = 0;
static unsigned long therapyDurationMs = THERAPY_DURATION_10_MIN;
static unsigned long patternStartMs = 0;
static unsigned long lastTickMs = 0;
static bool patternsInitialized = false;
static uint32_t therapySessionId = 0;


#define MAX_THERAPY_PATTERNS 30
static int patternSequence[MAX_THERAPY_PATTERNS];
static int totalPatterns = 0;
int currentPatternIndex = 0;
TrainingDelay currentTrainingDelay = TRAIN_INSTANT;
unsigned long therapyDuration = THERAPY_DURATION_10_MIN;

static unsigned long durationForSubMode(uint8_t idx) {
    switch (idx) {
        case 0: return THERAPY_DURATION_10_MIN;
        case 1: return THERAPY_DURATION_20_MIN;
        case 2: return THERAPY_DURATION_30_MIN;
        default: return THERAPY_DURATION_10_MIN;
    }
}

uint16_t getTherapyTotalPatternCount() {
    return (uint16_t)totalPatterns;
}

uint16_t getTherapyUniquePatternCount() {
    bool seen[PATTERN_COUNT] = {false};
    uint16_t unique = 0;
    for (int i = 0; i < totalPatterns; i++) {
        int p = patternSequence[i];
        if (p >= 0 && p < PATTERN_COUNT && !seen[p]) {
            seen[p] = true;
            unique++;
        }
    }
    return unique;
}

int getTherapyPatternSequence(uint8_t* out, uint8_t maxLen) {
    if (out == nullptr || maxLen == 0) return 0;
    int n = (totalPatterns < (int)maxLen) ? totalPatterns : (int)maxLen;
    for (int i = 0; i < n; i++) {
        out[i] = (uint8_t)patternSequence[i];
    }
    return n;
}

const char* getPatternNameByIndex(int idx) {
    if (idx < 0 || idx >= PATTERN_COUNT) return "Unknown";
    return PATTERN_NAMES[idx];
}

static void initializePatternSequence() {
    patternsInitialized = true;
    currentPatternIndex = 0;

    const unsigned long patternMs = THERAPY_PATTERN_MS;
    totalPatterns = (int)((therapyDurationMs + patternMs - 1) / patternMs);

    if (totalPatterns > MAX_THERAPY_PATTERNS) {
        totalPatterns = MAX_THERAPY_PATTERNS;
    }

    if (totalPatterns <= 0) {
        totalPatterns = 1;
    }

    for (int i = 0; i < totalPatterns; i++) {
        if (i < PATTERN_COUNT) {
            patternSequence[i] = i;
        } else {
            patternSequence[i] = random(0, PATTERN_COUNT);
        }
    }

    rtt.print("Pattern sequence initialized: ");
    rtt.print(totalPatterns);
    rtt.println(" patterns");

    for (int i = 0; i < totalPatterns; i++) {
        rtt.print("  Pattern ");
        rtt.print(i + 1);
        rtt.print(": ");
        rtt.println(PATTERN_NAMES[patternSequence[i]]);
    }
}

static void printTick(unsigned long now) {
    if (now - lastTickMs < 1000UL) return;
    lastTickMs = now;

    rtt.print("[Therapy] ");
    rtt.print((now - therapyStartMs) / 1000UL);
    rtt.print("s | Pattern ");
    rtt.print(currentPatternIndex + 1);
    rtt.print("/");
    rtt.print(totalPatterns);
    rtt.print(" [");
    rtt.print(therapyGetCurrentPatternName());
    rtt.print("] next: ");
    rtt.println(therapyGetNextPatternName());
}

static void patternMuscleActivation(unsigned long e) {
    const unsigned long span = THERAPY_PATTERN_MS;
    const unsigned long capped = (e < span) ? e : span;
    float pct = 30.0f + ((float)capped / (float)span) * 70.0f;
    motorSetDuty(constrain((int)((pct / 100.0f) * 255.0f), 0, 255));
}

static void patternReverseRamp(unsigned long e) {
    const unsigned long span = THERAPY_PATTERN_MS;
    const unsigned long capped = (e < span) ? e : span;
    float pct = 100.0f - ((float)capped / (float)span) * 70.0f;
    motorSetDuty(constrain((int)((pct / 100.0f) * 255.0f), 0, 255));
}

static void patternRampPattern(unsigned long e) {
    unsigned long c = e % 10200UL;
    int pwm = (c < 5100UL) ? map(c, 0, 5100, 0, 255) : map(c, 5100, 10200, 255, 0);
    motorSetDuty(constrain(pwm, 0, 255));
}

static void patternWaveTherapy(unsigned long e) {
    unsigned long c = e % 2400UL;
    if (c < 100) motorSetDuty(255);
    else if (c < 400) motorSetDuty(0);
    else if (c < 900) motorSetDuty(255);
    else if (c < 1200) motorSetDuty(0);
    else if (c < 2200) motorSetDuty(255);
    else motorSetDuty(0);
}

static void patternSlowWave(unsigned long e) {
    unsigned long c = e % 18000UL;
    int pwm = (c < 9000UL) ? map(c, 0, 9000, 50, 200) : map(c, 9000, 18000, 200, 50);
    motorSetDuty(constrain(pwm, 0, 255));
}

static void patternSinusoidalWave(unsigned long e) {
    float rad = (e / 1000.0f) * 360.0f / 2.0f * (float)PI / 180.0f;
    float s = (sinf(rad) + 1.0f) * 0.5f;
    int intensity = 50 + (int)(s * 150.0f);
    motorSetDuty(constrain(intensity, 0, 255));
}

static void patternTriangleWave(unsigned long e) {
    unsigned long c = e % 4000UL;
    int pwm = (c < 2000UL) ? map(c, 0, 2000, 60, 200) : map(c, 2000, 4000, 200, 60);
    motorSetDuty(constrain(pwm, 0, 255));
}

static void patternDoubleWave(unsigned long e) {
    float t = e / 1000.0f;
    float slow = (sinf(t * 360.0f / 4.0f * (float)PI / 180.0f) + 1.0f) * 0.5f;
    int base = 70 + (int)(slow * (255.0f - 70.0f));
    int ripple = (int)(30.0f * sinf(t * 360.0f / 0.5f * (float)PI / 180.0f));
    motorSetDuty(constrain(base + ripple, 0, 255));
}

static void patternAntiFatigue(unsigned long e) {
    unsigned long c = e % 5000UL;
    if (c < 4500UL) {
        motorSetDuty((c % 500UL < 200UL) ? 225 : 0);
    } else {
        motorSetDuty(0);
    }
}

static void patternPulseRamp(unsigned long e) {
    static const int intensities[9] = {120, 150, 180, 210, 240, 210, 180, 150, 120};
    unsigned long c = e % 9000UL;
    int step = (int)((c / 1000UL) % 9UL);
    motorSetDuty((c % 1000UL < 200UL) ? intensities[step] : 0);
}

static void patternInstantTripleBase(unsigned long e) {
    const unsigned long cycle = e % 10000UL;
    const int base = 100;
    const int pulse = 220;
    int out = base;
    if (cycle < 250UL) out = pulse;
    else if (cycle >= 750UL && cycle < 1000UL) out = pulse;
    else if (cycle >= 1500UL && cycle < 1750UL) out = pulse;
    motorSetDuty(out);
}

static void patternConstTriple(unsigned long e) {
    const unsigned long c = e % 10000UL;
    const int base = 140;
    const int pulse = 230;
    int out = base;
    if (c < 200UL) out = pulse;
    else if (c >= 500UL && c < 700UL) out = pulse;
    else if (c >= 800UL && c < 1000UL) out = pulse;
    motorSetDuty(out);
}

static void patternExpDoubleSine(unsigned long e) {
    const float cycle = 60000.0f;
    float t = ((float)(e % 60000UL) / cycle) * (2.0f * (float)PI);
    float expPart = expf(t / (2.0f * (float)PI)) / expf(1.0f);
    float s1 = (sinf(2.0f * t) + 1.0f) * 0.5f;
    float s2 = (sinf(3.0f * t) + 1.0f) * 0.5f;
    float mix = 0.5f * s1 + 0.5f * s2;
    int out = (int)(30.0f + expPart * mix * 225.0f);
    motorSetDuty(constrain(out, 0, 255));
}

static void patternBreathingExpSquare(unsigned long e) {
    const unsigned long c = e % 8000UL;
    const float half = 4000.0f;
    float env;
    if (c < 4000UL) {
        env = expf(((float)c) / half) / expf(1.0f);
    } else {
        env = expf(1.0f - (((float)c - half) / half)) / expf(1.0f);
    }
    bool gateHigh = ((c / 500UL) % 2UL) == 0UL;
    float gate = gateHigh ? 1.0f : 0.35f;
    int out = (int)(130.0f + env * (180.0f - 130.0f) * gate);
    motorSetDuty(constrain(out, 0, 255));
}

static void executePattern(int idx, unsigned long e) {
    switch (idx) {
        case PATTERN_MUSCLE_ACTIVATION: patternMuscleActivation(e); break;
        case PATTERN_REVERSE_RAMP: patternReverseRamp(e); break;
        case PATTERN_RAMP_PATTERN: patternRampPattern(e); break;
        case PATTERN_WAVE_THERAPY: patternWaveTherapy(e); break;
        case PATTERN_SLOW_WAVE: patternSlowWave(e); break;
        case PATTERN_SINUSOIDAL_WAVE: patternSinusoidalWave(e); break;
        case PATTERN_TRIANGLE_WAVE: patternTriangleWave(e); break;
        case PATTERN_DOUBLE_WAVE: patternDoubleWave(e); break;
        case PATTERN_ANTI_FATIGUE: patternAntiFatigue(e); break;
        case PATTERN_PULSE_RAMP: patternPulseRamp(e); break;
        case PATTERN_INSTANT_TRIPLE_BASE: patternInstantTripleBase(e); break;
        case PATTERN_CONST_TRIPLE: patternConstTriple(e); break;
        case PATTERN_EXP_DOUBLE_SINE: patternExpDoubleSine(e); break;
        case PATTERN_BREATH_EXP_SQUARE: patternBreathingExpSquare(e); break;
        default: motorSetDuty(0); break;
    }
}

void therapySetup() {
    motorSetup();
    therapyState = THERAPY_IDLE;
    patternsInitialized = false;
}

void therapyStart() {
    therapySessionId++;
    if (therapySessionId == 0) {
        therapySessionId = 1;
    }
    therapyDurationMs = durationForSubMode(therapySubModeIndex);
    therapyDuration = therapyDurationMs;
    therapyStartMs = millis();
    patternStartMs = therapyStartMs;
    lastTickMs = therapyStartMs;
    therapyState = THERAPY_RUNNING;
    patternsInitialized = false;
    currentPatternIndex = 0;
    initializePatternSequence();

    rtt.print("Therapy: starting — sub-mode ");
    rtt.print((int)therapySubModeIndex);
    rtt.print(" (");
    rtt.print((unsigned long)(therapyDurationMs / 60000UL));
    rtt.println(" min session)");
    onTherapyStarted();
}

void therapyStop(bool returnToTraining) {
    motorSetDuty(0);
    therapyState = THERAPY_IDLE;
    patternsInitialized = false;
    if (returnToTraining) {
        rtt.println("Therapy: session complete — switching to Training mode");
        setDeviceMode(MODE_TRAINING);
    }
    onTherapyEnded();
}

void therapyLoop() {
    if (isCalibrating()) {
        return;
    }
    if (therapyState != THERAPY_RUNNING) return;

    unsigned long now = millis();
    unsigned long totalElapsed = now - therapyStartMs;

    if (totalElapsed >= therapyDurationMs) {
        therapyStop();
        return;
    }

    if (!patternsInitialized) {
        initializePatternSequence();
        patternStartMs = now;
    }

    unsigned long patternElapsed = now - patternStartMs;
    if (patternElapsed >= THERAPY_PATTERN_MS) {
        currentPatternIndex++;
        patternStartMs = now;
        patternElapsed = 0;

        if (currentPatternIndex >= totalPatterns) {
            therapyStop();
            return;
        }

        rtt.print("Switching to pattern ");
        rtt.print(currentPatternIndex + 1);
        rtt.print(": ");
        rtt.println(therapyGetCurrentPatternName());
    }

    printTick(now);
    executePattern(patternSequence[currentPatternIndex], patternElapsed);
}

bool therapyIsRunning() {
    return currentMode == MODE_THERAPY && therapyState == THERAPY_RUNNING;
}

unsigned long therapyGetElapsedMs() {
    if (therapyState != THERAPY_RUNNING || currentMode != MODE_THERAPY) return 0;
    return millis() - therapyStartMs;
}

unsigned long therapyGetRemainingMs() {
    if (therapyState != THERAPY_RUNNING || currentMode != MODE_THERAPY) return 0;
    unsigned long elapsed = millis() - therapyStartMs;
    if (elapsed >= therapyDurationMs) return 0;
    return therapyDurationMs - elapsed;
}

const char* therapyGetCurrentPatternName() {
    if (!patternsInitialized || currentPatternIndex >= totalPatterns) return "Unknown";
    return PATTERN_NAMES[patternSequence[currentPatternIndex]];
}

const char* therapyGetNextPatternName() {
    if (!patternsInitialized || currentPatternIndex + 1 >= totalPatterns) return "Complete";
    return PATTERN_NAMES[patternSequence[currentPatternIndex + 1]];
}

unsigned long therapyGetElapsedSeconds() {
    return therapyGetElapsedMs() / 1000UL;
}

unsigned long therapyGetRemainingSeconds() {
    return therapyGetRemainingMs() / 1000UL;
}

unsigned long therapyGetTotalDurationSeconds() {
    if (!therapyIsRunning()) return 0;
    return therapyDurationMs / 1000UL;
}

unsigned long therapyGetPatternElapsedSeconds() {
    if (!therapyIsRunning()) return 0;
    return (millis() - patternStartMs) / 1000UL;
}

unsigned long therapyGetPatternRemainingSeconds() {
    if (!therapyIsRunning()) return 0;
    unsigned long elapsed = millis() - patternStartMs;
    if (elapsed >= THERAPY_PATTERN_MS) return 0;
    return (THERAPY_PATTERN_MS - elapsed) / 1000UL;
}

uint8_t therapyGetCurrentPatternIndex() {
    if (!therapyIsRunning() || currentPatternIndex < 0 || currentPatternIndex >= totalPatterns) {
        return 0;
    }
    return (uint8_t)(currentPatternIndex + 1); // 1-based for app
}

uint8_t therapyGetCurrentPatternId() {
    if (!therapyIsRunning() ||
        !patternsInitialized ||
        currentPatternIndex < 0 ||
        currentPatternIndex >= totalPatterns) {
        return 255;
    }
    return (uint8_t)patternSequence[currentPatternIndex];
}

uint8_t therapyGetTotalPatternCount() {
    return (uint8_t)totalPatterns;
}

uint32_t therapyGetSessionId() {
    if (!therapyIsRunning()) return 0;
    return therapySessionId;
}

