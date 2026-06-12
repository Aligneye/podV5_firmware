#pragma once

// ============================================================
//  AlignEye Firmware V4 — Board Pin Configuration
//  Custom nRF52832 PCB
// ============================================================

// ── Button ────────────────────────────────────────────────
#define PIN_BUTTON      11   // P0.11 → Arduino 11

// ── RGB LED ───────────────────────────────────────────────
#define PIN_LED_RED     13   // P0.13 → Arduino 13
#define PIN_LED_GREEN   14   // P0.14 → Arduino 14
#define PIN_LED_BLUE    15  // P0.15 → Arduino 15

// ── Motor ─────────────────────────────────────────────────
#define PIN_MOTOR       17   // P0.17 → Arduino 17

// ── I2C — LIS3DH Accelerometer ────────────────────────────
#define PIN_I2C_SDA     26   // SDA → P0.26
#define PIN_I2C_SCL     27   // SCL → P0.27

// ── Reset ─────────────────────────────────────────────────
// P0.21 → hardware reset, not used in firmware

// ── Battery ADC ───────────────────────────────────────────
#define PIN_BATTERY_ADC A0   // BATPER → P0.02/AIN0

// ── Button timing (ms) ────────────────────────────────────
#define DEBOUNCE_MS         50
#define DOUBLE_CLICK_GAP_MS 400
#define HOLD_MS             1000

// ── Vibration intensity (PWM 0-255) ───────────────────────
#define VIB_INTENSITY_LOW   160
#define VIB_INTENSITY_MID   200
#define VIB_INTENSITY_HIGH  230
#define VIB_INTENSITY_MAX   255

// ── Posture calibration (same defaults as ESP32 reference) ─────────────────
#define CALIB_THRESHOLD   2.0f
#define CALIB_MIN_SAMPLES 25

// ── Therapy durations (ms) ────────────────────────────────
#define THERAPY_DURATION_5_MIN   300000UL
#define THERAPY_DURATION_10_MIN  600000UL
#define THERAPY_DURATION_20_MIN  1200000UL
#define THERAPY_DURATION_30_MIN  1800000UL
#define THERAPY_PATTERN_MS       120000UL  // each pattern runs 2 min

// ── Bluetooth (App compatibility) ──────────────────────────
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_DEVICE_NAME         "align pod"

// ── RTT logging ─────────────────────────────────────────────────────────────
#ifndef ALIGN_RTT_JSON_LOG
#define ALIGN_RTT_JSON_LOG       0
#endif

#ifndef ALIGN_RTT_BLE_RX_LOG
#define ALIGN_RTT_BLE_RX_LOG     0
#endif

#ifndef ALIGN_RTT_STATUS_LOG
#define ALIGN_RTT_STATUS_LOG     1
#endif

#ifndef ALIGN_RTT_STATUS_INTERVAL_MS
#define ALIGN_RTT_STATUS_INTERVAL_MS 1000UL
#endif

#ifndef ALIGN_RTT_SENSOR_LOG
#define ALIGN_RTT_SENSOR_LOG     0
#endif

#ifndef ALIGN_RTT_CALIB_VERBOSE
#define ALIGN_RTT_CALIB_VERBOSE  0
#endif

#ifndef ALIGN_RTT_THERAPY_VERBOSE
#define ALIGN_RTT_THERAPY_VERBOSE 0
#endif

#ifndef ALIGN_RTT_SESSION_VERBOSE
#define ALIGN_RTT_SESSION_VERBOSE 0
#endif

// ── Therapy patterns (10 total) ───────────────────────────
enum TherapyPattern {
    PATTERN_MUSCLE_ACTIVATION = 0,  // always first
    PATTERN_REVERSE_RAMP,
    PATTERN_RAMP_PATTERN,
    PATTERN_WAVE_THERAPY,
    PATTERN_SLOW_WAVE,
    PATTERN_SINUSOIDAL_WAVE,
    PATTERN_TRIANGLE_WAVE,
    PATTERN_DOUBLE_WAVE,
    PATTERN_ANTI_FATIGUE,
    PATTERN_PULSE_RAMP,
    PATTERN_INSTANT_TRIPLE_BASE,
    PATTERN_CONST_TRIPLE,
    PATTERN_EXP_DOUBLE_SINE,
    PATTERN_BREATH_EXP_SQUARE,
    PATTERN_COUNT
};
