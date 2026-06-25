#include "training.h"
#include "button.h"
#include "calibration.h"
#include "motor.h"
#include "session_stats.h"
#include "step_count.h"
#include "storage.h"
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include "monitor_log.h"

/**
Ye code different modules ko connect kar raha hai.
training.h → training mode ke functions
button.h → current mode aur button-related variables
calibration.h → calibration chal rahi hai ya nahi
motor.h → vibration motor control
session_stats.h → training session start/end tracking
step_count.h → step counting
storage.h → saved calibration load/save
Adafruit_LIS3DH.h → LIS3DH accelerometer sensor library
Wire.h → I2C communication
math.h → angle calculation ke liye atan2f, fabsf
string.h → text copy ke liye strncpy

Matlab ye file posture training ka brain hai.
*/

extern RTTStream rtt;

// ── Spec constants ─────────────────────────────────────────────────────────
static constexpr float kLpfAlpha = 0.1f;
static constexpr float kMotionThreshold = 2.0f;
static constexpr float kDirectionDeg = 20.0f;
float kBadPostureDeg = 30.0f;
static constexpr float kAngleClampDeg = 90.0f;
static constexpr float kDefaultOriginY = 6.75f;
static constexpr float kDefaultOriginZ = 6.75f;
static constexpr float kNearZero = 0.1f;
static constexpr uint32_t kGoodDebounceMs = 100UL;
static constexpr int kInitMaxAttempts = 5;
static constexpr uint32_t kInitRetryDelayMs = 200UL;

/*
| Constant                   | Meaning | | -------------------------- |
------------------------------------------------------- | | `kLpfAlpha = 0.1` |
Sensor data smooth karne ke liye filter strength        | | `kMotionThreshold
= 2.0`   | Device movement detect karne ka threshold               | |
`kDirectionDeg = 20`       | 20° se zyada forward/backward direction decide hoti
hai | | `kBadPostureDeg = 25`      | 25° se zyada forward bend = bad posture |
| `kAngleClampDeg = 90`      | Angle ko ±90° ke andar limit karta hai | |
`kDefaultOriginY/Z = 6.75` | Default straight posture calibration value | |
`kNearZero = 0.1`          | Agar calibration zero ke near ho to invalid maanega
| | `kGoodDebounceMs = 100`    | Good posture confirm karne ke liye 100 ms
stable time   | | `kInitMaxAttempts = 5`     | Sensor start karne ke max
attempts                      | | `kInitRetryDelayMs = 200`  | Retry ke beech
200 ms delay                             |
*/

static Adafruit_LIS3DH lis = Adafruit_LIS3DH();

float rawX = 0, rawY = 0, rawZ = 0;
float Y_ORIGIN = kDefaultOriginY;
float Z_ORIGIN = kDefaultOriginZ;

float currentAngle = 0.0f;
bool isBadPosture = false;
bool sensorInitialized = false;
bool s_bootProfileDetectionDone = false;

char orientationText[16] = "UNKNOWN";
char directionText[16] = "UNKNOWN";
char postureText[96] = "UNKNOWN";

/*
| Variable            | Meaning                                     |
| ------------------- | ------------------------------------------- |
| `rawX/rawY/rawZ`    | Sensor se aaya latest raw data              |
| `Y_ORIGIN/Z_ORIGIN` | Straight posture ka calibrated reference    |
| `currentAngle`      | Current posture angle                       |
| `isBadPosture`      | Bad posture hai ya nahi                     |
| `sensorInitialized` | Sensor properly connected/start hua ya nahi |
| `orientationText`   | Device vertical hai ya inverted             |
| `directionText`     | Forward/backward/straight                   |
| `postureText`       | Good posture / bad posture text             |
*/

static bool _moving = false;

/** Filtered accel (α=0.1) — same seed rule as ESP32 posture_training.cpp */
static float g_fx = 0, g_fy = 0, g_fz = 0;
static bool s_lpfSeeded = false;

/** Motion deltas in updatePostureAngle — reset when origin changes so
 * recalibration does not glitch. */
static float s_motionPrevX = 0, s_motionPrevY = 0, s_motionPrevZ = 0;

/** Good-posture debounce timer — reset with new origin. */
static unsigned long s_goodPostureStableStart = 0;

/*
Simple explanation:

_moving → device move ho raha hai ya stable hai
g_fx/g_fy/g_fz → filtered/smoothed sensor values
s_lpfSeeded → filter first time initialize hua ya nahi
s_motionPrevX/Y/Z → previous reading for movement detection
s_goodPostureStableStart → good posture kab se stable hai

Low-pass filter ka matlab:
Sensor reading thodi noisy hoti hai. Filter us reading ko smooth karta hai.

Example:

Raw reading suddenly jump kare, to filter usse slowly change karega. Isse false
vibration alerts kam honge.
*/

static bool trainingIngestAccelSample(void) {
  if (!sensorInitialized) {
    static unsigned long lastInitRetryMs = 0;
    unsigned long now = millis();
    if (now - lastInitRetryMs >= 5000UL) {
      lastInitRetryMs = now;
      rtt.println("[Training] LIS3DH not initialized, retrying...");
      initPostureSensor(true); // quick retry
    }
    return false;
  }

  const unsigned long now = millis();
  static unsigned long lastSampleTimeMs = 0;
  // 100Hz ODR -> 10ms sampling interval
  if (now - lastSampleTimeMs < 10UL) {
    return false;
  }
  lastSampleTimeMs = now;

  sensors_event_t e;
  lis.getEvent(&e);

  // Detect sensor failure/disconnect (physically impossible to read exactly 0.0
  // on all axes)
  if (e.acceleration.x == 0.0f && e.acceleration.y == 0.0f &&
      e.acceleration.z == 0.0f) {
    static unsigned long lastFailPrintMs = 0;
    if (now - lastFailPrintMs >= 5000UL) {
      lastFailPrintMs = now;
      rtt.println("[Training] LIS3DH returned all zeros - connection lost or "
                  "sensor crashed!");
    }
    sensorInitialized = false;
    s_lpfSeeded = false;
    return false;
  }

  rawX = e.acceleration.x;
  rawY = e.acceleration.y;
  rawZ = e.acceleration.z;

  const uint32_t stepBefore = stepCountGetTotal();
  stepCountProcessSample(rawX, rawY, rawZ, now);
  const uint32_t stepAfter = stepCountGetTotal();
#if ALIGN_RTT_SENSOR_LOG
  if (!isCalibrating() && stepAfter > stepBefore) {
    rtt.print("[Step Trigger] count=");
    rtt.println((unsigned long)stepAfter);
  }
#else
  (void)stepBefore;
  (void)stepAfter;
#endif

  if (!s_lpfSeeded) {
    g_fx = rawX;
    g_fy = rawY;
    g_fz = rawZ;
    s_lpfSeeded = true;
  } else {
    g_fx = kLpfAlpha * rawX + (1.0f - kLpfAlpha) * g_fx;
    g_fy = kLpfAlpha * rawY + (1.0f - kLpfAlpha) * g_fy;
    g_fz = kLpfAlpha * rawZ + (1.0f - kLpfAlpha) * g_fz;
  }

  return true;
}

bool trainingSampleAccelForCalibration(void) {
  trainingIngestAccelSample();
  return sensorInitialized;
}

void trainingGetFilteredAccel(float *outY, float *outZ) {
  if (outY)
    *outY = g_fy;
  if (outZ)
    *outZ = g_fz;
}

// Session stats (training mode)
static Mode s_lastModeForSession = MODE_IDLE;

#if ALIGN_RTT_SENSOR_LOG
static unsigned long s_lastSensorRttMs = 0;
#endif
static unsigned long s_badMotorStartMs = 0;
static unsigned long s_vibToggleMs = 0;
static bool s_vibOn = false;

/** Motor uses same rule as ESP32 isBadPosture (forward > 25° + debounce). */
static bool s_forwardMotorBad = false;
static unsigned long s_trainingStartMs = 0;

static void loadStoredCalibration() {
  float loadedY = kDefaultOriginY;
  float loadedZ = kDefaultOriginZ;
  if (fabsf(loadedY) < kNearZero && fabsf(loadedZ) < kNearZero) {
    loadedY = kDefaultOriginY;
    loadedZ = kDefaultOriginZ;
  }
  Y_ORIGIN = loadedY;
  Z_ORIGIN = loadedZ;
}

void setPostureOrigin3D(float avgX, float avgY, float avgZ) {
  Y_ORIGIN = avgY;
  Z_ORIGIN = avgZ;

  /*
   * Recalibration only updates origin; without re-seeding, g_f* still reflect
   * the old low-pass state so angle vs the new origin is wrong until the filter
   * converges. Seed LPF from the calibration vector (x,y,z) and last raw; align
   * motion baseline.
   */
  g_fx = avgX;
  g_fy = avgY;
  g_fz = avgZ;
  s_lpfSeeded = true;
  s_motionPrevX = rawX;
  s_motionPrevY = rawY;
  s_motionPrevZ = rawZ;
  s_goodPostureStableStart = 0;
  isBadPosture = false;
}

void setPostureOrigin(float avgY, float avgZ) {
  if (fabsf(avgY) < kNearZero && fabsf(avgZ) < kNearZero) {
    avgY = kDefaultOriginY;
    avgZ = kDefaultOriginZ;
  }

  // Reconstruct X based on unit heuristic
  float magSq = avgY * avgY + avgZ * avgZ;
  float avgX = 0.0f;
  if (magSq < 2.0f) {
      float diff = 1.0f - magSq;
      avgX = (diff > 0.0f) ? sqrtf(diff) : 0.0f;
  } else {
      float diff = (9.80665f * 9.80665f) - magSq;
      avgX = (diff > 0.0f) ? sqrtf(diff) : 0.0f;
  }
  setPostureOrigin3D(avgX, avgY, avgZ);
}

static float computePostureAngle(float X, float Y, float Z) {
  const OrientationProfile* active = getActiveProfile();

  // Use fallback values if no profile is active
  float rx = 0.0f;
  float ry = Y_ORIGIN;
  float rz = Z_ORIGIN;

  if (active) {
    rx = active->refX;
    ry = active->refY;
    rz = active->refZ;

    // Set orientationText to profile name
    strncpy(orientationText, active->name, sizeof(orientationText) - 1);
    orientationText[sizeof(orientationText) - 1] = '\0';
  } else {
    strncpy(orientationText, "DEFAULT", sizeof(orientationText) - 1);
    orientationText[sizeof(orientationText) - 1] = '\0';
  }

  // 1. Normalize the reference vector (R)
  float magR = sqrtf(rx*rx + ry*ry + rz*rz);
  if (magR < 0.001f) return 0.0f;
  float vx = rx / magR;
  float vy = ry / magR;
  float vz = rz / magR;

  // 2. Normalize the current filtered accelerometer vector (C)
  float magC = sqrtf(X*X + Y*Y + Z*Z);
  if (magC < 0.001f) return 0.0f;
  float ax = X / magC;
  float ay = Y / magC;
  float az = Z / magC;

  // 3. Cosine component: D = C . R
  float dot = ax*vx + ay*vy + az*vz;

  // 4. Perpendicular depth component: pz = az - D * vz
  float pz = az - dot * vz;

  // 5. Isolate sagittal plane (normalize by projection plane magnitude)
  float planeMagSq = 1.0f - vz*vz;
  float a_d = pz;
  if (planeMagSq > 0.001f) {
    a_d = pz / sqrtf(planeMagSq);
  }

  // 6. Calculate relative angle
  float angle = atan2f(a_d, dot) * (180.0f / (float)M_PI);

  // 7. Clamp angle to safety limits
  if (angle > kAngleClampDeg) {
    angle = kAngleClampDeg;
  }
  if (angle < -kAngleClampDeg) {
    angle = -kAngleClampDeg;
  }

  return angle;
}

static void recoverI2CBus() {
  Wire.end();
  rtt.println("[Training] Attempting I2C bus recovery...");
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);

  if (digitalRead(PIN_I2C_SDA) == LOW) {
    rtt.println("[Training] SDA is stuck LOW. Toggling SCL...");
    pinMode(PIN_I2C_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
      digitalWrite(PIN_I2C_SCL, LOW);
      delayMicroseconds(5);
      digitalWrite(PIN_I2C_SCL, HIGH);
      delayMicroseconds(5);

      pinMode(PIN_I2C_SDA, INPUT_PULLUP);
      if (digitalRead(PIN_I2C_SDA) == HIGH) {
        rtt.println("[Training] SDA released!");
        break;
      }
    }
  }

  pinMode(PIN_I2C_SDA, OUTPUT);
  digitalWrite(PIN_I2C_SDA, LOW);
  pinMode(PIN_I2C_SCL, OUTPUT);
  digitalWrite(PIN_I2C_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_I2C_SDA, HIGH);
  delayMicroseconds(5);

  pinMode(PIN_I2C_SDA, INPUT);
  pinMode(PIN_I2C_SCL, INPUT);
}

void initPostureSensor(bool quick) {
  recoverI2CBus();
  Wire.setPins(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.begin();
  int maxAttempts = quick ? 1 : kInitMaxAttempts;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (lis.begin(0x18) || lis.begin(0x19)) {
      lis.setRange(LIS3DH_RANGE_2_G);
      lis.setDataRate(LIS3DH_DATARATE_100_HZ);
      sensorInitialized = true;
      s_lpfSeeded = false;
      loadStoredCalibration();
      rtt.println("LIS3DH: OK (±2G, 100Hz)");
      return;
    }
    if (!quick) {
      delay(kInitRetryDelayMs);
    }
  }
  sensorInitialized = false;
  s_lpfSeeded = false;
  if (!quick) {
    rtt.println("LIS3DH: init failed (will retry on wake)");
  }
}

bool updatePostureAngle() {
  if (!sensorInitialized)
    return false;

  if (!trainingIngestAccelSample()) {
    return false;
  }

  const float X = g_fx;
  const float Y = g_fy;
  const float Z = g_fz;

  const float dx = rawX - s_motionPrevX;
  const float dy = rawY - s_motionPrevY;
  const float dz = rawZ - s_motionPrevZ;
  s_motionPrevX = rawX;
  s_motionPrevY = rawY;
  s_motionPrevZ = rawZ;
  const float motionStrength = sqrtf(dx*dx + dy*dy + dz*dz);
  _moving = (motionStrength > kMotionThreshold);

  currentAngle = computePostureAngle(X, Y, Z);

  if (currentAngle > kDirectionDeg) {
    strncpy(directionText, "FORWARD", sizeof(directionText) - 1);
  } else if (currentAngle < -kDirectionDeg) {
    strncpy(directionText, "BACKWARD", sizeof(directionText) - 1);
  } else {
    strncpy(directionText, "STRAIGHT", sizeof(directionText) - 1);
  }
  directionText[sizeof(directionText) - 1] = '\0';

  const char *baseText =
      (currentAngle > kBadPostureDeg || currentAngle < -kBadPostureDeg)
          ? "BAD POSTURE"
          : "GOOD POSTURE";

  if (currentMode == MODE_TRAINING && isTrainingActive()) {
    /* Same layout as ESP32 posture_training: "GOOD POSTURE [S1:N …]" */
    snprintf(postureText, sizeof(postureText), "%s [S1:%lu %lus %lu-bad]",
             baseText, (unsigned long)getTrainingSessionNumber(),
             (unsigned long)getTrainingSessionDurationSec(),
             (unsigned long)getTrainingSessionBadPostureCount());
  } else {
    strncpy(postureText, baseText, sizeof(postureText) - 1);
    postureText[sizeof(postureText) - 1] = '\0';
  }

  const uint32_t nowMs = millis();

  /* ESP32: bad if angle > 25 only; good after stable <= 25 for > 100 ms */
  if (currentAngle > kBadPostureDeg) {
    isBadPosture = true;
    s_goodPostureStableStart = 0;
  } else {
    if (s_goodPostureStableStart == 0) {
      s_goodPostureStableStart = nowMs;
    }
    if ((nowMs - s_goodPostureStableStart) > kGoodDebounceMs) {
      isBadPosture = false;
    }
  }

  s_forwardMotorBad = isBadPosture;
  return true;
}

static void logTrainingSensorRtt(uint32_t now) {
#if ALIGN_RTT_SENSOR_LOG
  if (now - s_lastSensorRttMs < 1000UL)
    return;
  s_lastSensorRttMs = now;

  if (!sensorInitialized) {
    rtt.println(
        "[Training] LIS3DH not connected — check I2C (SDA P0.26 / SCL P0.27)");
    return;
  }

  rtt.print("[Training] raw m/s² X=");
  rtt.print(rawX, 2);
  rtt.print(" Y=");
  rtt.print(rawY, 2);
  rtt.print(" Z=");
  rtt.print(rawZ, 2);
  rtt.print(" | angle=");
  rtt.print(currentAngle, 1);
  rtt.print("° ");
  rtt.print(orientationText);
  rtt.print(" ");
  rtt.print(directionText);
  rtt.print(" moving=");
  rtt.print(_moving ? "Y" : "N");
  rtt.print(" | ");
  rtt.print(postureText);
  rtt.print(" | sub=");
  rtt.print(trainingSubModes[static_cast<uint8_t>(trainingSubModeIndex)]);
  rtt.print(" | steps=");
  rtt.println((unsigned long)stepCountGetTotal());
#else
  (void)now;
#endif
}

/**
 * Instant: pulse after 200 ms sustained forward-bad.
 * Delayed: pulse after 5 s.
 * No alerts: motor off.
 */
static void applyTrainingMotorFeedback(uint32_t now) {
  // Keep calibration haptics authoritative (start/ticks/fail/success windows).
  if (calibrationMotorActive()) {
    s_badMotorStartMs = 0;
    s_vibOn = false;
    return;
  }

  // Give a 1-second grace period at the start of training session
  if (now - s_trainingStartMs < 1000UL) {
    motorSetDuty(0);
    s_badMotorStartMs = 0;
    s_vibOn = false;
    s_vibToggleMs = 0;
    return;
  }

  if (static_cast<uint8_t>(trainingSubModeIndex) >= TRAINING_SUBMODE_COUNT) {
    trainingSubModeIndex = TrainingAlertStyle::Instant;
  }

  if (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) {
    motorSetDuty(0);
    s_badMotorStartMs = 0;
    return;
  }

  if (!s_forwardMotorBad) {
    motorSetDuty(0);
    s_badMotorStartMs = 0;
    s_vibOn = false;
    s_vibToggleMs = 0;
    return;
  }

  if (s_badMotorStartMs == 0) {
    s_badMotorStartMs = now;
  }

  const unsigned long delayMs = (trainingSubModeIndex == TrainingAlertStyle::Instant) ? 200UL : 5000UL;
  if ((now - s_badMotorStartMs) < delayMs) {
    motorSetDuty(0);
    return;
  }

  const unsigned long vibInterval = 500UL;
  if ((now - s_vibToggleMs) >= vibInterval) {
    s_vibToggleMs = now;
    s_vibOn = !s_vibOn;
  }
  motorSetDuty(s_vibOn ? VIB_INTENSITY_MAX : 0);
}

void sleepPostureSensor() {
  if (sensorInitialized) {
    lis.setDataRate(LIS3DH_DATARATE_POWERDOWN);
  }
}

void wakePostureSensor() {
  if (!sensorInitialized) {
    initPostureSensor();
    return;
  }
  lis.setDataRate(LIS3DH_DATARATE_100_HZ);
}

bool isDeviceMoving() { return _moving; }

uint32_t getDeviceStepCount() { return stepCountGetTotal(); }

void trainingStart() {
  rtt.println("Training: start");
  s_trainingStartMs = millis();
  onTrainingStarted();
}

void trainingStop() {
  motorSetDuty(0);
  s_badMotorStartMs = 0;
  s_vibOn = false;
  s_vibToggleMs = 0;
  s_forwardMotorBad = false;
  rtt.println("Training: stop");
  onTrainingEnded();
}

void trainingSetup() {
  stepCountInit();
  initPostureSensor();
}

static bool isOrientationDetectionReady(uint32_t now) {
  if (now < 2000UL) {
    return false;
  }

  static unsigned long stableStartMs = 0;
  static float prevX = 0.0f, prevY = 0.0f, prevZ = 0.0f;
  static bool firstCheck = true;

  if (firstCheck) {
    prevX = rawX;
    prevY = rawY;
    prevZ = rawZ;
    firstCheck = false;
    return false;
  }

  float dx = rawX - prevX;
  float dy = rawY - prevY;
  float dz = rawZ - prevZ;
  prevX = rawX;
  prevY = rawY;
  prevZ = rawZ;

  float motion = sqrtf(dx*dx + dy*dy + dz*dz);
  constexpr float STABILITY_THRESHOLD = 0.5f;

  if (motion < STABILITY_THRESHOLD) {
    if (stableStartMs == 0) {
      stableStartMs = now;
    }
    if (now - stableStartMs >= 1000UL) {
      return true;
    }
  } else {
    stableStartMs = 0; // reset
  }

  return false;
}

void trainingLoop() {
  const uint32_t now = millis();

  static bool s_prevCalibrating = false;
  const bool calibrating = isCalibrating();
  if (s_prevCalibrating && !calibrating) {
    s_badMotorStartMs = 0;
    s_vibToggleMs = 0;
    s_vibOn = false;
  }
  s_prevCalibrating = calibrating;

  if (calibrating) {
    return;
  }

  bool sampleReady = false;
  if (currentMode == MODE_TRAINING) {
    if (s_lastModeForSession != MODE_TRAINING) {
      wakePostureSensor();
      trainingStart();
      s_lastModeForSession = MODE_TRAINING;
    }
    sampleReady = updatePostureAngle();
    applyTrainingMotorFeedback(now);
    logTrainingSensorRtt(now);
  } else {
    if (s_lastModeForSession == MODE_TRAINING) {
      trainingStop();
    }
    s_lastModeForSession = currentMode;

    if (sensorInitialized) {
      sampleReady = trainingIngestAccelSample();
    }
  }

  (void)sampleReady;
}
