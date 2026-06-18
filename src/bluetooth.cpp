#include "bluetooth.h"
#include "calibration.h"
#include "therapy.h"
#include "button.h"
#include "training.h"
#include "motor.h"
#include "device_time.h"
#include "session_stats.h"
#include "BatteryMonitor.h"
#include <bluefruit.h>
#include <ble_hci.h>
#if __has_include(<InternalFileSystem.h>)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#define BLE_PAIR_STORE_HAS_FS 1
#else
#define BLE_PAIR_STORE_HAS_FS 0
#endif

int therapyIntensityLevel = 2; // Default to Mid (2)

extern RTTStream rtt;

static bool connected = false;
static bool bleInitialized = false;
static uint16_t currentConnHandle = BLE_CONN_HANDLE_INVALID;
static bool pairingUnlockActive = true;
static bool blePairingKnownPaired = false;
static bool clearBondsAfterDisconnect = false;
static bool connectionHapticPending = false;
static bool connectionHapticPlayed = false;
static bool disconnectionHapticPending = false;
static bool forceTelemetrySync = false;
static bool forceLiveSync = false;
static unsigned long lastLiveSendMs = 0;
static unsigned long connectedSinceMs = 0;

static BLEService gService(BLE_SERVICE_UUID);
static BLECharacteristic gCharacteristic(BLE_CHARACTERISTIC_UUID);
static BLECharacteristic *pCharacteristic = nullptr;
static BatteryMonitor batteryMonitor(PIN_BATTERY_ADC);
static float batteryVoltage = 0.0f;
static uint16_t batteryRawAdc = 0;
static uint16_t batterySenseMillivolts = 0;
static uint16_t batteryMillivolts = 0;
static uint8_t batteryPercentage = 0;
static unsigned long lastBatteryReadMs = 0;
static bool batteryReadValid = false;
static bool batteryBlinkActive = false;
static unsigned long batteryBlinkStartMs = 0;

static constexpr uint32_t BATTERY_BLINK_PERIOD_MS = 1000UL;
static constexpr uint8_t BATTERY_BLINK_COUNT = 5;
static constexpr uint32_t UNPAIRED_RED_BLINK_PERIOD_MS = 160UL;
static constexpr uint32_t CONNECTION_HAPTIC_DELAY_MS = 800UL;
static constexpr uint8_t DISCONNECTION_HAPTIC_DUTY = 150;
static constexpr uint16_t DISCONNECTION_HAPTIC_MS = 250;
static const char* BLE_PAIR_MARKER_PATH = "/ble_pair.dat";

static const char* bleDisconnectReasonText(uint8_t reason) {
    switch (reason) {
        case BLE_HCI_CONNECTION_TIMEOUT:
            return "timeout";
        case BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION:
            return "remote_user";
        case BLE_HCI_REMOTE_DEV_TERMINATION_DUE_TO_LOW_RESOURCES:
            return "remote_low_resources";
        case BLE_HCI_REMOTE_DEV_TERMINATION_DUE_TO_POWER_OFF:
            return "remote_power_off";
        case BLE_HCI_LOCAL_HOST_TERMINATED_CONNECTION:
            return "local_host";
        case BLE_HCI_CONN_INTERVAL_UNACCEPTABLE:
            return "conn_interval_unacceptable";
        case BLE_HCI_CONN_TERMINATED_DUE_TO_MIC_FAILURE:
            return "mic_failure";
        default:
            return "unknown";
    }
}

static void setRgbLedPwm(uint8_t red, uint8_t green, uint8_t blue) {
    analogWrite(PIN_LED_RED, 255 - red);
    analogWrite(PIN_LED_GREEN, 255 - green);
    analogWrite(PIN_LED_BLUE, 255 - blue);
}

static void turnRgbLedOff() {
    setRgbLedPwm(0, 0, 0);
}

static bool loadBlePairMarker() {
#if BLE_PAIR_STORE_HAS_FS
    InternalFS.begin();
    File file = InternalFS.open(BLE_PAIR_MARKER_PATH, FILE_O_READ);
    if (!file) return false;

    uint8_t marker = 0;
    int readBytes = file.read(&marker, sizeof(marker));
    file.close();
    return readBytes == (int)sizeof(marker) && marker == 1u;
#else
    return false;
#endif
}

static void saveBlePairMarker(bool paired) {
#if BLE_PAIR_STORE_HAS_FS
    InternalFS.begin();
    InternalFS.remove(BLE_PAIR_MARKER_PATH);
    if (!paired) return;

    File file = InternalFS.open(BLE_PAIR_MARKER_PATH, FILE_O_WRITE);
    if (!file) return;

    uint8_t marker = 1u;
    file.write(&marker, sizeof(marker));
    file.flush();
    file.close();
#else
    (void)paired;
#endif
}

static void updateBatteryLed(uint8_t percentage, uint8_t brightness = 255) {
    if (percentage >= 67) {
        setRgbLedPwm(0, brightness, 0);
    } else if (percentage >= 34) {
        setRgbLedPwm(brightness, brightness, 0);
    } else {
        setRgbLedPwm(brightness, 0, 0);
    }
}

static void updateBatteryReading(unsigned long now) {
    if (batteryReadValid && (now - lastBatteryReadMs) < 5000UL) {
        return;
    }

    BatteryReading reading = batteryMonitor.readBattery();
    batteryRawAdc = reading.rawAdc;
    batterySenseMillivolts = reading.senseMillivolts;
    batteryMillivolts = reading.batteryMillivolts;
    batteryPercentage = reading.percentage;
    batteryVoltage = batteryMillivolts / 1000.0f;
    lastBatteryReadMs = now;
    batteryReadValid = true;
}

static void updateBatteryStatusBlink(unsigned long now) {
    if (!batteryBlinkActive) {
        return;
    }

    if (batteryBlinkStartMs == 0UL) {
        batteryBlinkStartMs = now;
    }

    updateBatteryReading(now);

    const unsigned long elapsed = now - batteryBlinkStartMs;
    const unsigned long totalBlinkMs = (unsigned long)BATTERY_BLINK_COUNT * BATTERY_BLINK_PERIOD_MS;
    if (elapsed >= totalBlinkMs) {
        batteryBlinkActive = false;
        turnRgbLedOff();
        return;
    }

    const unsigned long phaseMs = elapsed % BATTERY_BLINK_PERIOD_MS;
    uint8_t brightness = 0;
    if (phaseMs < (BATTERY_BLINK_PERIOD_MS / 2UL)) {
        brightness = (uint8_t)((phaseMs * 255UL) / (BATTERY_BLINK_PERIOD_MS / 2UL));
    } else {
        brightness = (uint8_t)(((BATTERY_BLINK_PERIOD_MS - phaseMs) * 255UL) / (BATTERY_BLINK_PERIOD_MS / 2UL));
    }

    if (brightness == 0) {
        turnRgbLedOff();
    } else {
        updateBatteryLed(batteryPercentage, brightness);
    }
}

static bool updatePairingLed(unsigned long now) {
    if (!connected) {
        const bool redOn = ((now / (UNPAIRED_RED_BLINK_PERIOD_MS / 2UL)) % 2UL) == 0UL;
        setRgbLedPwm(redOn ? 255 : 0, 0, 0);
        return true;
    }

    return false;
}

static void startAdvertising() {
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();

    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(gService);
    Bluefruit.ScanResponse.addName();

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244); // 20ms fast, 152.5ms slow
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0); // Advertise forever
}

static void onBleConnect(uint16_t conn_handle) {
    currentConnHandle = conn_handle;
    connected = true;
    connectionHapticPending = true;
    connectionHapticPlayed = false;
    disconnectionHapticPending = false;
    forceTelemetrySync = true;
    forceLiveSync = true;
    lastLiveSendMs = 0;
    connectedSinceMs = millis();
    turnRgbLedOff();
    rtt.println("BLE: Connected");
}

static void onBleDisconnect(uint16_t conn_handle, uint8_t reason) {
    (void)conn_handle;
    connected = false;
    currentConnHandle = BLE_CONN_HANDLE_INVALID;
    connectionHapticPending = false;
    connectionHapticPlayed = false;
    disconnectionHapticPending = true;
    forceLiveSync = false;
    connectedSinceMs = 0UL;
    rtt.print("BLE: Disconnected reason=0x");
    rtt.print(reason, HEX);
    rtt.print(" (");
    rtt.print(bleDisconnectReasonText(reason));
    rtt.println(")");

    if (clearBondsAfterDisconnect) {
        clearBondsAfterDisconnect = false;
        Bluefruit.Periph.clearBonds();
    }

    startAdvertising();
}

static void onBleSecured(uint16_t conn_handle) {
    (void)conn_handle;
    blePairingKnownPaired = true;
    pairingUnlockActive = false;
    connectionHapticPending = true;
    rtt.println("BLE: Paired/Secured");
}

static void applyTrainingTiming(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    TrainingAlertStyle previous = trainingSubModeIndex;
    if (value == "INSTANT") {
        trainingSubModeIndex = TrainingAlertStyle::Instant; // Instant
        rtt.println("BLE CMD: POSTURE_TIMING=INSTANT");
    } else if (value == "DELAYED") {
        trainingSubModeIndex = TrainingAlertStyle::Delayed; // Delayed
        rtt.println("BLE CMD: POSTURE_TIMING=DELAYED");
    } else if (value == "AUTOMATIC") {
        trainingSubModeIndex = TrainingAlertStyle::Instant; // Fallback to Instant
        rtt.println("BLE CMD: POSTURE_TIMING=AUTOMATIC");
    }

    if (currentMode == MODE_TRAINING && trainingSubModeIndex != previous) {
        markSubModeChanged();
    }
}

static void applyTherapyDurationMinutes(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    int mins = value.toInt();
    if (mins <= 0) return;

    uint8_t previous = therapySubModeIndex;
    if (mins == 10) {
        therapySubModeIndex = 0;
    } else if (mins == 20) {
        therapySubModeIndex = 1;
    } else if (mins == 30) {
        therapySubModeIndex = 2;
    } else {
        therapySubModeIndex = 0; // default 10 min
    }
    if (therapySubModeIndex != previous) {
        if (currentMode == MODE_THERAPY) {
            therapyStop(false);
            markSubModeChanged();
        }
    }
    rtt.printf("BLE CMD: THERAPY_DURATION_MIN=%d\n", mins);
}

static void applyMode(const String &valueRaw) {
    if (isCalibrating()) {
        rtt.println("BLE CMD: MODE change ignored - calibration in progress");
        return;
    }

    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    Mode previousMode = currentMode;
    if (value == "TRACKING") {
        deviceOn = true;
        TrainingAlertStyle previous = trainingSubModeIndex;
        trainingSubModeIndex = TrainingAlertStyle::NoAlerts; // No alerts (equivalent to tracking)
        setDeviceMode(MODE_TRAINING);
        if (previousMode == MODE_TRAINING && currentMode == MODE_TRAINING && trainingSubModeIndex != previous) {
            markSubModeChanged();
        }
        rtt.println("BLE CMD: MODE=TRACKING");
    } else if (value == "TRAINING" || value == "POSTURE") {
        deviceOn = true;
        TrainingAlertStyle previous = trainingSubModeIndex;
        if (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) {
            trainingSubModeIndex = TrainingAlertStyle::Instant; // Default back to Instant if it was tracking
        }
        setDeviceMode(MODE_TRAINING);
        if (previousMode == MODE_TRAINING && currentMode == MODE_TRAINING && trainingSubModeIndex != previous) {
            markSubModeChanged();
        }
        rtt.println("BLE CMD: MODE=TRAINING");
    } else if (value == "THERAPY") {
        deviceOn = true;
        setDeviceMode(MODE_THERAPY);
        rtt.println("BLE CMD: MODE=THERAPY");
    }
}

static void applyCalibrationControl(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    if (value == "START") {
        if (isCalibrating()) return;
        calibrationRequestStart();
        rtt.println("BLE CMD: CALIBRATION START");
    } else if (value == "CANCEL") {
        if (!isCalibrating()) return;
        calibrationRequestCancel();
        rtt.println("BLE CMD: CALIBRATION CANCEL");
    }
}

static void applyAction(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    if (value == "CALIBRATE") {
        if (isCalibrating()) return;
        calibrationRequestStart();
        rtt.println("BLE CMD: ACTION=CALIBRATE");
    } else if (value == "CALIBRATE_CANCEL") {
        if (!isCalibrating()) return;
        calibrationRequestCancel();
        rtt.println("BLE CMD: ACTION=CALIBRATE_CANCEL");
    } else if (value == "PROFILE_CLEAR" || value == "CLEAR_PROFILES") {
        clearCalibrationProfiles();
        rtt.println("BLE CMD: ACTION=PROFILE_CLEAR");
    }
}

static void applyTimeSync(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    long epoch = value.toInt();
    if (epoch > 0) {
        setDeviceTime(epoch);
        rtt.printf("BLE CMD: TIME=%ld\n", epoch);
    }
}

static void applyTZOffset(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    long tz = value.toInt();
    setDeviceTZOffset(tz);
    rtt.printf("BLE CMD: TZ=%ld\n", tz);
}

static void applyTherapyIntensity(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    int level = value.toInt();
    if (level >= 1 && level <= 3) {
        therapyIntensityLevel = level;
        rtt.printf("BLE CMD: THERAPY_INTENSITY=%d\n", level);
    }
}

static void applyDifficultyDegrees(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    int deg = value.toInt();
    if (deg >= 5 && deg <= 60) {
        kBadPostureDeg = (float)deg;
        rtt.printf("BLE CMD: DIFFICULTY_DEG=%d\n", deg);
    }
}

static void applyProfileSelection(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    if (value.length() == 0) return;

    String upper = value;
    upper.toUpperCase();
    if (upper == "CLEAR" || upper == "RESET") {
        clearCalibrationProfiles();
        rtt.println("BLE CMD: PROFILE=CLEAR");
        return;
    }

    if (upper == "DEFAULT") {
        selectDefaultCalibrationProfile();
        rtt.println("BLE CMD: PROFILE=DEFAULT");
        return;
    }

    int requestedIndex = value.toInt();
    if (requestedIndex > 0) {
        if (selectCalibrationProfile((uint8_t)(requestedIndex - 1))) {
            rtt.printf("BLE CMD: PROFILE_INDEX=%d\n", requestedIndex);
        } else {
            rtt.printf("BLE CMD: PROFILE_INDEX=%d ignored\n", requestedIndex);
        }
        return;
    }

    for (uint8_t i = 0; i < getProfileCount(); i++) {
        const OrientationProfile *profile = getProfile(i);
        if (!profile) continue;
        String profileName = String(profile->name);
        profileName.toUpperCase();
        if (upper == profileName) {
            selectCalibrationProfile(i);
            rtt.printf("BLE CMD: PROFILE=%s\n", profile->name);
            return;
        }
    }

    rtt.printf("BLE CMD: PROFILE=%s ignored\n", value.c_str());
}

static void parseAndApplyBleCommand(const String &payloadRaw) {
    String payload = payloadRaw;
    payload.trim();
    if (payload.length() == 0) return;

    String requestedMode = "";
    int start = 0;
    int payloadLen = payload.length();
    while (start < payloadLen) {
        int end = payload.indexOf(';', start);
        if (end < 0) {
            end = payload.length();
        }

        String token = payload.substring(start, end);
        token.trim();

        int sep = token.indexOf('=');
        if (sep > 0) {
            String key = token.substring(0, sep);
            String value = token.substring(sep + 1);
            key.trim();
            key.toUpperCase();
            value.trim();

            if (key == "MODE") {
                requestedMode = value;
            } else if (key == "POSTURE_TIMING") {
                applyTrainingTiming(value);
            } else if (key == "THERAPY_DURATION_MIN") {
                applyTherapyDurationMinutes(value);
            } else if (key == "THERAPY_INTENSITY") {
                applyTherapyIntensity(value);
            } else if (key == "DIFFICULTY_DEG") {
                applyDifficultyDegrees(value);
            } else if (key == "PROFILE" || key == "PROFILE_INDEX" || key == "PROFILES") {
                applyProfileSelection(value);
            } else if (key == "CALIBRATE" || key == "CALIBRATION") {
                applyCalibrationControl(value);
            } else if (key == "ACTION") {
                applyAction(value);
            } else if (key == "TIME") {
                applyTimeSync(value);
            } else if (key == "TZ") {
                applyTZOffset(value);
            }
        }

        start = end + 1;
    }

    if (requestedMode.length() > 0) {
        applyMode(requestedMode);
    }
}

static void onCharacteristicWrite(uint16_t conn_handle, BLECharacteristic *chr, uint8_t *data, uint16_t len) {
    (void)conn_handle;
    (void)chr;
    if (data == nullptr || len == 0) return;

    String payload;
    payload.reserve(len);
    for (uint16_t i = 0; i < len; i++) {
        payload += (char)data[i];
    }

#if ALIGN_RTT_BLE_RX_LOG
    rtt.print("BLE RX CMD: ");
    rtt.println(payload);
#endif
    parseAndApplyBleCommand(payload);
}

void bluetoothSetup() {
    rtt.print("Initializing BLE as: ");
    rtt.println(BLE_DEVICE_NAME);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);
    turnRgbLedOff();
    batteryMonitor.begin();
    updateBatteryReading(millis());
    blePairingKnownPaired = loadBlePairMarker();
    pairingUnlockActive = !blePairingKnownPaired;

    if (!bleInitialized) {
        Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
        Bluefruit.begin(1, 0);
        Bluefruit.autoConnLed(false); // Disable auto LED blinking on PIN_LED1/PIN_LED2 (P0.17/P0.19)
        Bluefruit.setName(BLE_DEVICE_NAME);
        Bluefruit.setTxPower(4); // dBm
        Bluefruit.Periph.setConnectCallback(onBleConnect);
        Bluefruit.Periph.setDisconnectCallback(onBleDisconnect);
        Bluefruit.Security.setSecuredCallback(onBleSecured);

        gService.begin();

        gCharacteristic.setProperties(
            CHR_PROPS_NOTIFY | CHR_PROPS_READ | CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
        gCharacteristic.setPermission(SECMODE_ENC_NO_MITM, SECMODE_ENC_NO_MITM);
        gCharacteristic.setMaxLen(512);
        gCharacteristic.setWriteCallback(onCharacteristicWrite);
        gCharacteristic.begin();
        gCharacteristic.write("{}");

        pCharacteristic = &gCharacteristic;
        bleInitialized = true;
    }

    startAdvertising();
}

void bluetoothLoop() {
    if (!pCharacteristic) return;

    unsigned long now = millis();
    if (connected && connectionHapticPending && !connectionHapticPlayed &&
        connectedSinceMs != 0UL &&
        (now - connectedSinceMs) >= CONNECTION_HAPTIC_DELAY_MS) {
        connectionHapticPending = false;
        connectionHapticPlayed = true;
        motorCancelFeedback();
        motorSetDuty(0);
        motorOverrideDuty(150, 125);
    }
    if (disconnectionHapticPending && !calibrationMotorActive()) {
        disconnectionHapticPending = false;
        motorCancelFeedback();
        motorSetDuty(0);
        motorOverrideDuty(DISCONNECTION_HAPTIC_DUTY, DISCONNECTION_HAPTIC_MS);
    }

    if (!updatePairingLed(now)) {
        updateBatteryStatusBlink(now);
    }

    // Determine submode string
    char subModeStr[16];
    if (currentMode == MODE_TRAINING) {
        if (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) {
            strcpy(subModeStr, "INSTANT"); // Fallback/default app expectation
        } else if (trainingSubModeIndex == TrainingAlertStyle::Instant) {
            strcpy(subModeStr, "INSTANT");
        } else {
            strcpy(subModeStr, "DELAYED");
        }
    } else if (currentMode == MODE_THERAPY) {
        snprintf(subModeStr, sizeof(subModeStr), "%lu MIN", therapyDuration / 60000);
    } else {
        strcpy(subModeStr, "INSTANT");
    }

    const OrientationProfile *activeProfile = getActiveProfile();
    const char *profileName = activeProfile ? activeProfile->name : "DEFAULT";

    const char *modeString = "TRACKING";
    if (currentMode == MODE_TRAINING) {
        modeString = (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) ? "TRACKING" : "TRAINING";
    } else if (currentMode == MODE_THERAPY) {
        modeString = "THERAPY";
    } else if (currentMode == MODE_OFF) {
        modeString = "OFF";
    }

    updateBatteryReading(now);

    if (connected && currentMode == MODE_TRAINING && !isCalibrating() &&
        (forceLiveSync || (now - lastLiveSendMs) >= 150UL)) {
        updatePostureAngle();
    }

    const char *appPosture =
        (currentAngle > kBadPostureDeg || currentAngle < -kBadPostureDeg)
            ? "BAD POSTURE"
            : "GOOD POSTURE";

    if (connected) {
        static unsigned long lastTelemetrySend = 0;
        static bool telemetryCacheValid = false;
        static char lastMode[12] = "";
        static char lastSubMode[16] = "";
        static char lastProfile[32] = "";
        static uint8_t lastBatteryPercentage = 255;

        bool telemetryChanged = forceTelemetrySync || !telemetryCacheValid ||
            strcmp(lastMode, modeString) != 0 ||
            strcmp(lastSubMode, subModeStr) != 0 ||
            strcmp(lastProfile, profileName) != 0 ||
            lastBatteryPercentage != batteryPercentage;

        if (!isCalibrating() &&
            (telemetryChanged || (now - lastTelemetrySend) >= 5000UL)) {
            char telemetryBuffer[128];
            snprintf(telemetryBuffer, sizeof(telemetryBuffer),
                "{\"t\":\"T\",\"mode\":\"%s\",\"sub_mode\":\"%s\","
                "\"profile\":\"%s\",\"battery\":%u}",
                modeString,
                subModeStr,
                profileName,
                (unsigned)batteryPercentage
            );
            pCharacteristic->write(telemetryBuffer);
            pCharacteristic->notify(telemetryBuffer);
#if ALIGN_RTT_JSON_LOG
            rtt.println(telemetryBuffer);
#endif

            strncpy(lastMode, modeString, sizeof(lastMode) - 1);
            lastMode[sizeof(lastMode) - 1] = '\0';
            strncpy(lastSubMode, subModeStr, sizeof(lastSubMode) - 1);
            lastSubMode[sizeof(lastSubMode) - 1] = '\0';
            strncpy(lastProfile, profileName, sizeof(lastProfile) - 1);
            lastProfile[sizeof(lastProfile) - 1] = '\0';
            lastBatteryPercentage = batteryPercentage;
            telemetryCacheValid = true;
            forceTelemetrySync = false;
            lastTelemetrySend = now;
        }

        if (forceLiveSync || (now - lastLiveSendMs) >= 150UL) {
            char liveBuffer[64];
            snprintf(liveBuffer, sizeof(liveBuffer),
                "{\"t\":\"L\",\"angle\":%.2f,\"posture\":\"%s\"}",
                currentAngle,
                appPosture
            );
            pCharacteristic->write(liveBuffer);
            pCharacteristic->notify(liveBuffer);
#if ALIGN_RTT_JSON_LOG
            rtt.println(liveBuffer);
#endif
            forceLiveSync = false;
            lastLiveSendMs = now;
        }
    }

    // Clean human-readable RTT status for day-to-day debugging.
#if ALIGN_RTT_STATUS_LOG
    static unsigned long lastStatusMs = 0;
    if (!isCalibrating() && (now - lastStatusMs) >= ALIGN_RTT_STATUS_INTERVAL_MS) {
        lastStatusMs = now;
        rtt.printf("STATUS mode=%s profile=%s profiles=%u angle=%s deg dir=%s posture=%s bad=%s batt_mv=%u batper_mv=%u batt_adc=%u batt_pct=%u steps=%lu\n",
                   modeString,
                   profileName,
                   (unsigned)getProfileCount(),
                   String(currentAngle, 1).c_str(),
                   directionText,
                   postureText,
                   isBadPosture ? "Y" : "N",
                   batteryMillivolts,
                   batterySenseMillivolts,
                   batteryRawAdc,
                   batteryPercentage,
                   (unsigned long)getDeviceStepCount());
    }
#endif

}

void bluetoothStartAdvertising() {
    rtt.println("BLE: start advertising");
    startAdvertising();
}

void bluetoothStopAdvertising() {
    rtt.println("BLE: stop advertising");
    Bluefruit.Advertising.stop();
}

bool bluetoothIsConnected() {
    return connected;
}

void bluetoothUnlockForPairing() {
    rtt.println("BLE: unlock pairing - clearing bonds");

    batteryBlinkActive = false;
    pairingUnlockActive = true;
    blePairingKnownPaired = false;
    clearBondsAfterDisconnect = false;
    connectionHapticPending = false;
    connectionHapticPlayed = false;
    disconnectionHapticPending = false;
    connectedSinceMs = 0UL;
    saveBlePairMarker(false);

    Bluefruit.Advertising.restartOnDisconnect(true);

    if (currentConnHandle != BLE_CONN_HANDLE_INVALID && Bluefruit.connected(currentConnHandle)) {
        clearBondsAfterDisconnect = true;
        Bluefruit.disconnect(currentConnHandle);
        return;
    }

    Bluefruit.Periph.clearBonds();
    startAdvertising();
}

void bluetoothRequestCalibrationStart() {
    calibrationRequestStart();
}

void bluetoothRequestBatteryStatusBlink() {
    batteryBlinkActive = true;
    batteryBlinkStartMs = 0UL;
}

void notifyNewSessionStored() {
    // Placeholder
}
