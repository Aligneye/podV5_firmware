1. 

training.h
Yeh ek C++ Header file (.h) hai jiska kaam classes, functions, aur variables ko define karna aur pure project ko batana hai ki training system mein kya-kya components uplabdh hain.

Lines 1–5: Header Guards & Libraries Include
cpp
#pragma once
#include <Arduino.h>
#include <RTTStream.h>
#include "config.h"
#pragma once: compiler ko batata hai ki is file ko compile karte waqt sirf ek hi baar include kiya jaye taaki duplicate declaration ki errors na aayen.
<Arduino.h>: Basic Arduino features aur types (jaise uint32_t) ke liye zaroori hai.
<RTTStream.h>: J-Link RTT logger debugger library hai jo debug prints ke liye use ho rahi hai.
"config.h": Global configurations aur settings (jaise modes aur pin allocations) ko import karta hai.
Lines 7–11: Core Session Controls
cpp
void trainingSetup();
void trainingLoop();
void trainingStart();
void trainingStop();
trainingSetup(): Device boot ke waqt LIS3DH sensor aur step counters ko setup karne ka declare function hai.
trainingLoop(): Main thread (main.cpp) ke loop mein lagatar call hone wala background process hai.
trainingStart() / trainingStop(): Session stats shuru karne aur pure subsystem ko on/off karne ke core functions hain.
Lines 13–17: Posture & Sensor Setup Functions
cpp
/** Call when user sets upright reference from accelerometer (Y,Z in m/s²). */
void setPostureOrigin(float y, float z);
void initPostureSensor(bool quick = false);
void updatePostureAngle();
setPostureOrigin(float y, float z): Jab user ekdum seedha khada ho kar button daba kar calibrate karta hai, tab hum current gravitational values y aur z pass karte hain jo dynamic posture baseline (Origin) ban jaati hai.
initPostureSensor(bool quick = false): Accelerometer ko start up par configure karta hai. quick = true par yeh bina zyada time delay liye fast retry karta hai.
updatePostureAngle(): Live measurement loop function hai jo sensor se raw angles fetch karta hai aur compute karta hai.
Lines 19–24: Filtering, Sleep & State Checks
cpp
/** Read LIS3DH + LPF for calibration (any mode). Returns false if sensor missing. */
bool trainingSampleAccelForCalibration(void);
void trainingGetFilteredAccel(float* outY, float* outZ);
void sleepPostureSensor();
void wakePostureSensor();
bool isDeviceMoving();
trainingSampleAccelForCalibration(): Bina kisi specific posture evaluation ke sirf accelerometer values update karne ke liye use hota hai (primarily calibration window ke waqt).
trainingGetFilteredAccel(...): Low-pass filtered $Y$ aur $Z$ axes output pointers ke through return karta hai.
sleepPostureSensor() / wakePostureSensor(): Power saving ke liye sensor ko sleep (POWERDOWN mode) par daalne aur jagane ke kaam aate hain.
isDeviceMoving(): Kya user dynamic state mein hai (chal/daud raha hai) check karta hai.
Lines 26–30: Session Data API (Getters)
cpp
bool     isTrainingSessionActive();
uint32_t getTrainingSessionNumber();
uint32_t getTrainingSessionDurationSec();
uint32_t getTrainingSessionBadPostureCount();
uint32_t getDeviceStepCount();
Yeh saare functions dusre modules (jaise Bluetooth ya display) ko current session ke statistics retrieve karne ke liye read-only interface provide karte hain.
Lines 32–42: Global Extern Variables
cpp
extern float rawX, rawY, rawZ;
extern float Y_ORIGIN, Z_ORIGIN;
extern float currentAngle;
extern bool  isBadPosture;
extern bool  sensorInitialized;
extern float kBadPostureDeg;
/** Short text for RTT / BLE (no Arduino String). */
extern char orientationText[16];
extern char directionText[16];
extern char postureText[96];
extern: Matlab in variables ko declare toh header mein kiya gaya hai, par inka actual space aur definitions training.cpp mein reserved hain.
orientationText, directionText, postureText: Raw static standard char arrays hain taaki Bluetooth (BLE) aur serial logger is dynamic text ko directly bina dynamic heap memory loss ke read kar sakein.
2. 

training.cpp
Yeh file main system business logic contain karti hai. Isko niche functions aur static code blocks ke roop mein split karke samjhte hain:

Lines 1–14: Includes and External Declarations
Yahan header files include ki gayi hain.
extern RTTStream rtt: SEGGER RTT channel debugger standard output ko access karne ke liye declare kiya gaya hai.
Lines 16–27: Static & Configuration Constants
cpp
static constexpr float kLpfAlpha = 0.1f;
static constexpr float kMotionThreshold = 2.0f;
static constexpr float kDirectionDeg = 20.0f;
float kBadPostureDeg = 25.0f;
static constexpr float kAngleClampDeg = 90.0f;
static constexpr float kDefaultOriginY = 6.75f;
static constexpr float kDefaultOriginZ = 6.75f;
static constexpr float kNearZero = 0.1f;
static constexpr uint32_t kGoodDebounceMs = 100UL;
static constexpr int kInitMaxAttempts = 5;
static constexpr uint32_t kInitRetryDelayMs = 200UL;
kLpfAlpha: Low pass filter coefficient hai. $0.1$ ka matlab hai ki naya acceleration sample sirf $10%$ weight rakhega, aur pehla state $90%$ weight rakhega, jisse sudden jump ya shake ignore ho sake.
kMotionThreshold: $2.0\text{ m/s}^2$ ka change. Isse bade changes motion detect karte hain.
kBadPostureDeg = 25.0f: Target angle limit hai, isse upar body bend hote hi trigger chala jayega.
kDefaultOriginY & kDefaultOriginZ: Agar calibration database empty ho, toh initial vertical seedha angle lagbhag $\sim 6.75\text{ m/s}^2$ assume hota hai.
Lines 29–53: Internal Static Variables
static Adafruit_LIS3DH lis = Adafruit_LIS3DH(): Adafruit hardware control object register ho raha hai jo I2C protocols run karega.
g_fx, g_fy, g_fz: Filtered values maintain karne ke liye variables hain.
s_goodPostureStableStart: Jab bad posture se user wapas theek posture mein aata hai, tab timer shuru karne ke liye standard benchmark timer.
Lines 55–106: 

trainingIngestAccelSample
Yeh function sabse pehle check karta hai ki sensor initialized hai ya nahi. Agar initialization fail ho chuki hai, toh har 5 seconds ke interval par reset karne ki koshish karega.
Sensor crash check: if (e.acceleration.x == 0.0f && e.acceleration.y == 0.0f && e.acceleration.z == 0.0f).
Agr sabhi axes standard exact $0.0$ aate hain, toh sensor crash ya wire loose hone ka andesha hota hai. Is state mein system error print karega aur initialization flag reset karega.
Step Counter Integration: stepCountProcessSample(rawX, rawY, rawZ, millis()) call hota hai jo dynamic acceleration se user ke running/walking steps nikalta hai.
Low Pass Filtering:
cpp
g_fx = kLpfAlpha * rawX + (1.0f - kLpfAlpha) * g_fx;
Is formula se smooth gravity direction maintain hoti hai taaki body tilting measurements stable aayen.
Lines 108–118: Calibration Helpers
trainingSampleAccelForCalibration(): Raw readings read karta hai aur operation validation code return karta hai.
trainingGetFilteredAccel(float *outY, float *outZ): Clean, filtered accelerometer coordinates extract karne ke liye external API call hai.
Lines 135–152: 

loadStoredCalibration
Device start hone par, yeh Flash memory ya EEPROM (storageLoadCalibration) se save kiya hua calibration origin load karta hai.
Agar flash data valid nahi hai (ya lagbhag zero ke paas hai), toh yeh default standard settings (kDefaultOriginY, kDefaultOriginZ) ko read/load karta hai taaki code crash na ho.
Lines 154–178: 

setPostureOrigin
cpp
void setPostureOrigin(float avgY, float avgZ) {
  // ... check near zero limits ...
  Y_ORIGIN = fabsf(avgY);
  Z_ORIGIN = avgZ;
  storageSaveCalibration(Y_ORIGIN, Z_ORIGIN);
  
  g_fx = rawX;
  g_fy = avgY;
  g_fz = avgZ;
  // ... reset movement delta and posture flags ...
}
Jab custom target posture origin lock kiya jata hai, tab yeh origin change parameters Y_ORIGIN aur Z_ORIGIN ko save karta hai.
LPF Re-seeding: Jab calibration change hoti hai, toh hum LPF variables (g_fx, g_fy, g_fz) ko live value par forced override kar dete hain. Agar aisa na karein, toh user ke naye calibrated posture ko update hone mein filter response slow hone ke karan thoda time lag jayega.
Lines 180–203: 

computePostureAngle
cpp
static float computePostureAngle(float Y, float Z) {
  const bool isVertical = (Y > 0.0f);
  // sets orientationText to "VERTICAL" or "INVERTED"
  const float effY = isVertical ? Y : -Y;
  const float currentAngleAbs = atan2f(Z, effY) * (180.0f / (float)M_PI);
  const float originAngleAbs = atan2f(Z_ORIGIN, Y_ORIGIN) * (180.0f / (float)M_PI);
  float angle = currentAngleAbs - originAngleAbs;
  
  // Clamping angle to range of [-90, +90]
  // ...
  return angle;
}
Yeh basic trigonometry use karta hai: $Angle = \text{atan2}(Z, Y)$ aur use degree mein convert karta hai ($180/\pi$ se multiply karke).
Live angle aur standard calibration angle ke gap se body tilting angle mil jata hai.
Use max $\pm90^{\circ}$ limit par restrict (clamp) kiya jata hai.
Lines 205–226: 

initPostureSensor
I2C pins define karta hai: Wire.setPins(PIN_I2C_SDA, PIN_I2C_SCL) aur Wire.begin() karta hai.
Adafruit library call: lis.begin(0x18) ya retry 0x19 address par call karta hai.
Accelerometer properties:
setRange(LIS3DH_RANGE_2_G): Sensibility range $\pm 2\text{G}$ rakhta hai (jo tilt sensing ke liye optimal hai).
setDataRate(LIS3DH_DATARATE_100_HZ): Sample updating rate $100\text{Hz}$ rakhta hai (i.e. har 10ms par naya data).
Lines 228–296: 

updatePostureAngle
Sensor data fetch karta hai.
Motion Detection:
cpp
const float dx = fabsf(rawX - s_motionPrevX);
// ...
const float motionStrength = dx + dy + dz;
_moving = (motionStrength > kMotionThreshold);
Yeh difference calculate karta hai ki pichli raw values aur abhi ki raw values mein kitna badlaav aaya. Agar dynamic movement ka sum $2.0\text{ m/s}^2$ se upar hai, toh isDeviceMoving status dynamic mark ho jata hai.
Angle direction mapping:
Angle $> 20^{\circ}$: FORWARD (Aage jhukaav).
Angle $< -20^{\circ}$: BACKWARD (Peeche jhukaav).
Otherwise: STRAIGHT (Seedha).
Bad Posture Debouncing:
Agar angle $> 25^{\circ}$ hai, toh target state isBadPosture true ho jayegi.
Lekin agar angle limit ke andar hai, toh timer s_goodPostureStableStart dekhega ki kya continuously $100\text{ms}$ tak posture good hai. Is debouncing se body ke chote shakes aur step walking vibrations ke chalte motor baar-baar trigger nahi hoti.
Lines 298–329: 

logTrainingSensorRtt
RTT prints logging function hai jo serial monitor/debugger par har $1000\text{ms}$ (1 second) ke interval par current status print karta hai (jaise angles, orientation, step counts, direction, mode).
Lines 331–376: 

applyTrainingMotorFeedback
Vibration feedback system ko run karta hai:
Agar calibration motor active hai, toh training motor control bypass rehti hai taaki calibration ke notifications clash na karein.
SubMode 2 (No Alerts): Motor output off (motorSetDuty(0)) kar deta hai.
SubMode 0 (Instant Alert - 200ms delay) & SubMode 1 (Delayed Alert - 5000ms delay):
Jaise hi isBadPosture detect hota hai, timer start ho jata hai.
Agar bad posture duration target submode limit (200ms ya 5000ms) cross kar jata hai, toh vibration pulse generator initiate hota hai.
Vibration toggle logic: s_vibOn = !s_vibOn (500ms intervals par motor turn on aur turn off hoti hai taaki user ko dynamic beep pulse touch feedback feel ho).
Lines 378–392: Sensor sleep & wake and motion state query
sleepPostureSensor(): LIS3DH accelerometer ko low power down state par set karta hai.
wakePostureSensor(): Accelerometer ko $100\text{Hz}$ rate par wake-up karta hai.
isDeviceMoving(): Return karta hai ki kya device walk/run action mein active hai.
Lines 394–425: Session stats get APIs & Session control
trainingStart(): Naye session ke liye parameters reset karta hai aur bluetooth/notification state onTrainingStarted() callback run karta hai.
trainingStop(): Session values reset karta hai, motor off karta hai aur onTrainingEnded() callback fire karta hai.
Lines 427–463: Setup & Loop background tasks
trainingSetup(): System initialization ke time calls dispatch karta hai.
trainingLoop():
cpp
if (currentMode == MODE_TRAINING) {
    // ... if not already initialized training session, start it ...
    updatePostureAngle();
    applyTrainingMotorFeedback(now);
    logTrainingSensorRtt(now);
} else {
    // ... stop training and clean parameters ...
}
Yeh loop har update cycle par tab execute hota hai jab system training mode (MODE_TRAINING) mein hota hai. Yeh sequence mein angles update karta hai, check karta hai ki motor vibrations lagani hain ya nahi, aur logger stats print karta hai.