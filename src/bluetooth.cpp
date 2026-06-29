#include "bluetooth.h"
#include "calibration.h"
#include "therapy.h"
#include "button.h"
#include "training.h"
#include "motor.h"
#include "storage.h"
#include "device_time.h"
#include "session_stats.h"
#include "BatteryMonitor.h"
#include "version.h"
#include "monitor_log.h"
#include <cstring>
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
static unsigned long lastTrainingTelemetrySendMs = 0;
static unsigned long lastTherapyLiveSendMs = 0;

static bool telemetryCacheValid = false;
static char lastMode[12] = "";
static char lastSubMode[16] = "";
static char lastProfile[32] = "";
static uint32_t lastProfileId = 0u;
static uint8_t lastBatteryPercentage = 255;
static bool lastRunning = false;
static unsigned long connectedSinceMs = 0;
static bool therapyPlanSentForSession = false;
static uint32_t lastTherapyPlanSessionId = 0;
static volatile bool pendingDfuEnter = false;
static volatile bool pendingProfileListSend = false;
static volatile bool pendingFactoryReset = false;

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

static constexpr uint32_t LIVE_PACKET_INTERVAL_MS = 150UL;
static constexpr uint32_t TRAINING_TELEMETRY_INTERVAL_MS = 1000UL;
static constexpr uint32_t THERAPY_LIVE_PACKET_INTERVAL_MS = 1000UL;
static constexpr uint32_t BATTERY_BLINK_PERIOD_MS = 1000UL;
static constexpr uint8_t BATTERY_BLINK_COUNT = 5;
static constexpr uint32_t UNPAIRED_RED_BLINK_PERIOD_MS = 160UL;
static constexpr uint32_t CONNECTION_HAPTIC_DELAY_MS = 800UL;
static constexpr uint8_t DISCONNECTION_HAPTIC_DUTY = 150;
static constexpr uint16_t DISCONNECTION_HAPTIC_MS = 250;
static const char* BLE_PAIR_MARKER_PATH = "/ble_pair.dat";

static void updateBatteryReading(unsigned long now);

static void rttPrintBlePacket(const char* direction, const char* payload) {
    if (!direction || !payload) return;
    rtt.print("[BLE ");
    rtt.print(direction);
    rtt.print("] ");
    rtt.println(payload);
}

static void sendBlePacket(const char* payload) {
    if (!pCharacteristic || !payload) return;
    pCharacteristic->write(payload);
    pCharacteristic->notify(payload);
    rttPrintBlePacket("TX", payload);
}

static void sendCommandAck(uint32_t seq, const char* cmd, bool ok, const char* error = nullptr) {
    if (!cmd || cmd[0] == '\0') return;
    char payload[192];
    if (ok) {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"ACK\",\"seq\":%lu,\"cmd\":\"%s\",\"ok\":true}",
                 (unsigned long)seq, cmd);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"ACK\",\"seq\":%lu,\"cmd\":\"%s\",\"ok\":false,\"error\":\"%s\"}",
                 (unsigned long)seq, cmd, error ? error : "UNKNOWN_ERROR");
    }
    sendBlePacket(payload);
}

static void sendDeviceInfoPacket() {
    if (!pCharacteristic || !connected) return;
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"INFO\",\"fw\":\"%s\",\"hw\":\"%s\",\"serial\":\"AEPOD0001\",\"protocol\":2,\"max_profiles\":8}",
             FW_VERSION,
             HW_VERSION);
    sendBlePacket(payload);
}

static void sendTherapyPlanPacket() {
    if (!pCharacteristic || !connected || !therapyIsRunning()) return;

    uint8_t seq[30];
    int total = getTherapyPatternSequence(seq, 30);

    char seqStr[128];
    int pos = 0;
    pos += snprintf(seqStr + pos, sizeof(seqStr) - pos, "[");
    for (int i = 0; i < total; i++) {
        pos += snprintf(seqStr + pos, sizeof(seqStr) - pos, "%d%s", (int)seq[i], (i == total - 1) ? "" : ",");
    }
    pos += snprintf(seqStr + pos, sizeof(seqStr) - pos, "]");

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"TP\",\"sid\":%lu,\"duration\":%lu,\"pattern_dur\":%lu,\"total\":%d,\"seq\":%s}",
             (unsigned long)therapyGetSessionId(),
             (unsigned long)therapyGetTotalDurationSeconds(),
             (unsigned long)(THERAPY_PATTERN_MS / 1000UL),
             total,
             seqStr);
    sendBlePacket(payload);
}

static void sendTherapyLivePacket() {
    if (!pCharacteristic || !connected || !therapyIsRunning() || isCalibrating()) {
        return;
    }

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"TL\",\"sid\":%lu,\"idx\":%d,\"pid\":%d,\"elapsed\":%lu,\"remaining\":%lu,\"p_elapsed\":%lu,\"p_remaining\":%lu}",
             (unsigned long)therapyGetSessionId(),
             (int)therapyGetCurrentPatternIndex(),
             (int)therapyGetCurrentPatternId(),
             (unsigned long)therapyGetElapsedSeconds(),
             (unsigned long)therapyGetRemainingSeconds(),
             (unsigned long)therapyGetPatternElapsedSeconds(),
             (unsigned long)therapyGetPatternRemainingSeconds());
    sendBlePacket(payload);
}

void notifyTherapyComplete(uint32_t elapsedSeconds) {
    if (!pCharacteristic || !connected) {
        return;
    }

    uint8_t seq[30];
    int total = getTherapyPatternSequence(seq, 30);

    char seqStr[128];
    int pos = 0;
    pos += snprintf(seqStr + pos, sizeof(seqStr) - pos, "[");
    for (int i = 0; i < total && pos > 0 && pos < ((int)sizeof(seqStr) - 2); i++) {
        pos += snprintf(seqStr + pos, sizeof(seqStr) - pos,
                        "%d%s", (int)seq[i], (i == total - 1) ? "" : ",");
    }
    if (pos < 0 || pos >= (int)sizeof(seqStr)) {
        pos = (int)sizeof(seqStr) - 2;
    }
    snprintf(seqStr + pos, sizeof(seqStr) - pos, "]");

    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"TC\",\"sid\":%lu,\"completed\":true,\"elapsed\":%lu,"
             "\"duration\":%lu,\"pattern_dur\":%lu,\"total\":%d,"
             "\"unique\":%u,\"seq\":%s}",
             (unsigned long)therapyGetSessionId(),
             (unsigned long)elapsedSeconds,
             (unsigned long)therapyGetTotalDurationSeconds(),
             (unsigned long)(THERAPY_PATTERN_MS / 1000UL),
             total,
             (unsigned)getTherapyUniquePatternCount(),
             seqStr);
    sendBlePacket(payload);
    forceTelemetrySync = true;
    forceLiveSync = true;
}

static const char* currentModeText() {
    if (currentMode == MODE_TRAINING) {
        return "TRAINING";
    }
    if (currentMode == MODE_THERAPY) {
        return "THERAPY";
    }
    return "IDLE";
}

static void sendLivePacket() {
    if (!pCharacteristic || !connected) return;
    if (isCalibrating()) return;

    updatePostureAngle();

    const char* postureStr = (currentAngle > kBadPostureDeg || currentAngle < -kBadPostureDeg)
        ? "BAD POSTURE"
        : "GOOD POSTURE";

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"L\",\"mode\":\"%s\",\"angle\":%.2f,\"difficulty_angle\":%.0f,\"posture\":\"%s\"}",
             currentModeText(),
             currentAngle,
             kBadPostureDeg,
             postureStr);

    sendBlePacket(payload);
}

static void sendStatusTelemetry(const char* source = nullptr, const char* reason = nullptr) {
    (void)source;
    (void)reason;
    if (!pCharacteristic || !connected) return;
    if (isCalibrating()) return;

    const unsigned long now = millis();

    // Determine submode string
    char subModeStr[16];
    if (currentMode == MODE_TRAINING) {
        if (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) {
            strcpy(subModeStr, "INSTANT");
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
    const uint32_t profileId = activeProfile ? activeProfile->id : 0u;
    const char *profileName = activeProfile ? activeProfile->name : "DEFAULT";

    const char *modeString = currentModeText();

    updateBatteryReading(now);

    const bool runningNow = therapyIsRunning();

    char telemetryBuffer[192];
    snprintf(
      telemetryBuffer,
      sizeof(telemetryBuffer),
      "{\"t\":\"T\",\"mode\":\"%s\",\"sub_mode\":\"%s\","
      "\"running\":%s,"
      "\"sid\":%lu,"
      "\"duration\":%lu,"
      "\"profile_id\":%lu,\"profile\":\"%s\",\"battery\":%u}",
      modeString,
      subModeStr,
      runningNow ? "true" : "false",
      (unsigned long)therapyGetSessionId(),
      therapyGetTotalDurationSeconds(),
      (unsigned long)profileId,
      profileName,
      (unsigned)batteryPercentage
    );
    sendBlePacket(telemetryBuffer);

    // Update state caches
    strncpy(lastMode, modeString, sizeof(lastMode) - 1);
    lastMode[sizeof(lastMode) - 1] = '\0';
    strncpy(lastSubMode, subModeStr, sizeof(lastSubMode) - 1);
    lastSubMode[sizeof(lastSubMode) - 1] = '\0';
    strncpy(lastProfile, profileName, sizeof(lastProfile) - 1);
    lastProfile[sizeof(lastProfile) - 1] = '\0';
    lastProfileId = profileId;
    lastBatteryPercentage = batteryPercentage;
    lastRunning = runningNow;
    telemetryCacheValid = true;
}



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
    (void)now;
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
    lastTrainingTelemetrySendMs = 0;
    lastTherapyLiveSendMs = 0;
    connectedSinceMs = millis();
    therapyPlanSentForSession = false;
    lastTherapyPlanSessionId = 0;
    turnRgbLedOff();
    rtt.println("[BLE EVT] connected");
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
    rtt.print("[BLE EVT] disconnected reason=0x");
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
    rtt.println("[BLE EVT] secured");
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
        logEvent("BLE", "mode_training");
    } else if (value == "THERAPY") {
        deviceOn = true;
        setDeviceMode(MODE_THERAPY);
        logEvent("BLE", "mode_therapy");
    }
}

static void stopTherapyFromBle() {
    if (currentMode == MODE_THERAPY) {
        setDeviceMode(MODE_TRAINING);
    } else {
        therapyStop(false);
    }
    deviceOn = true;
    forceTelemetrySync = true;
    forceLiveSync = true;
    logEvent("BLE", "therapy_stop");
}

static void applyCalibrationControl(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    if (value == "START") {
        if (isCalibrating()) return;
        calibrationRequestStart();
        logEvent("BLE", "calibration_start");
    } else if (value == "CANCEL") {
        if (!isCalibrating()) return;
        calibrationRequestCancel();
        logEvent("BLE", "calibration_cancel");
    }
}

static void applyAction(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    if (value == "CALIBRATE") {
        if (isCalibrating()) return;
        deviceOn = true;
        startCalibration();
        logEvent("BLE", "action_calibrate");
    } else if (value == "CALIBRATE_CANCEL") {
        if (!isCalibrating()) return;
        calibrationRequestCancel();
        logEvent("BLE", "action_calibrate_cancel");
    } else if (value == "STOP_THERAPY" || value == "THERAPY_STOP" ||
               value == "STOP THERAPY" || value == "STOP") {
        stopTherapyFromBle();
    } else if (value == "ENTER_DFU" || value == "OTA_DFU" || value == "DFU") {
        if (pendingDfuEnter) return;
        pendingDfuEnter = true;
        notifyDfuStatus("armed");
        logEvent("BLE", "action_enter_dfu");
    } else if (value == "DEVICE_INFO" || value == "GET_VERSION" || value == "VERSION" || value == "GET_DEVICE_INFO") {
        sendDeviceInfoPacket();
        logEvent("BLE", "action_device_info");
    } else if (value == "PROFILE_CLEAR" || value == "CLEAR_PROFILES") {
        clearCalibrationProfiles();
        logEvent("BLE", "action_profile_clear");
    } else if (value == "FACTORY_RESET") {
        pendingFactoryReset = true;
        logEvent("BLE", "action_factory_reset");
    }
}

static void sendProfileList() {
    if (!pCharacteristic || !connected) return;
    char payload[1024];
    char profilesJson[800];
    profilesJson[0] = '\0';
    strncat(profilesJson, "[", sizeof(profilesJson) - 1);
    bool activeProfileSent = false;
    bool defaultProfileSent = false;
    for (uint8_t i = 0; i < getProfileCount(); i++) {
        const OrientationProfile* p = getProfile(i);
        if (!p || !p->valid) continue;
        int quality = (int)(p->stabilityScore);
        if (quality < 0) quality = 0;
        if (quality > 100) quality = 100;
        
        int isActiveVal = 0;
        if (!activeProfileSent && getActiveProfile() && getActiveProfile()->id == p->id) {
            isActiveVal = 1;
            activeProfileSent = true;
        }
        
        int isDefaultVal = 0;
        if (!defaultProfileSent && getDefaultProfileId() == p->id) {
            isDefaultVal = 1;
            defaultProfileSent = true;
        }
        
        char item[128];
        snprintf(item, sizeof(item),
                 "%s{\"id\":%lu,\"s\":%u,\"n\":\"%s\",\"a\":%d,\"d\":%d,\"c\":%lu,\"q\":%d}",
                 (profilesJson[1] == '\0') ? "" : ",",
                 (unsigned long)p->id,
                 (unsigned)(i + 1),
                 p->name,
                 isActiveVal,
                 isDefaultVal,
                 (unsigned long)p->createdAtEpoch,
                 quality);
        strncat(profilesJson, item, sizeof(profilesJson) - strlen(profilesJson) - 1);
    }
    strncat(profilesJson, "]", sizeof(profilesJson) - strlen(profilesJson) - 1);
    snprintf(payload, sizeof(payload), "{\"t\":\"P\",\"profiles\":%s,\"count\":%u,\"max\":8}", profilesJson, (unsigned)getProfileCount());
    sendBlePacket(payload);
}

static void sendCalibrationDone(bool success, uint32_t profileId = 0, const char* name = nullptr, uint8_t slot = 0, uint16_t quality = 0, uint16_t samples = 0, const char* reason = nullptr, float refX = 0.0f, float refY = 0.0f, float refZ = 0.0f, uint16_t passedSamples = 0) {
    if (!pCharacteristic || !connected) return;
    char payload[256];
    if (success) {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"C\",\"state\":\"DONE\",\"result\":\"success\",\"profile_id\":%lu,\"name\":\"%s\",\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"total_samples\":%u,\"passed_samples\":%u}",
                 (unsigned long)profileId, name ? name : "", refX, refY, refZ, (unsigned)samples, (unsigned)passedSamples);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"C\",\"state\":\"DONE\",\"result\":\"failed\",\"reason\":\"%s\"}",
                 reason ? reason : "MOVEMENT_TOO_HIGH");
    }
    sendBlePacket(payload);
}

void notifyCalibrationComplete(bool success, uint32_t profileId, const char* name, uint8_t slot, uint16_t quality, uint16_t sampleCount, const char* reason, float refX, float refY, float refZ, uint16_t passedSamples) {
    sendCalibrationDone(success, profileId, name, slot, quality, sampleCount, reason, refX, refY, refZ, passedSamples);
}

static void applyTimeSync(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    long epoch = value.toInt();
    if (epoch > 0) {
        setDeviceTime(epoch);
        char payload[48];
        snprintf(payload, sizeof(payload), "{\"time\":%ld}", epoch);
        logPacket("BLE", payload);
    }
}

static void applyTZOffset(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    long tz = value.toInt();
    setDeviceTZOffset(tz);
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"tz\":%ld}", tz);
    logPacket("BLE", payload);
}

static void applyTherapyIntensity(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    int level = value.toInt();
    if (level >= 1 && level <= 3) {
        therapyIntensityLevel = level;
        char payload[48];
        snprintf(payload, sizeof(payload), "{\"therapy_intensity\":%d}", level);
        logPacket("BLE", payload);
    }
}

static void applyDifficultyDegrees(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    int deg = value.toInt();
    if (deg >= 5 && deg <= 60) {
        kBadPostureDeg = (float)deg;
        char payload[48];
        snprintf(payload, sizeof(payload), "{\"difficulty_deg\":%d}", deg);
        logPacket("BLE", payload);
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
        logEvent("BLE", "profile_clear");
        return;
    }

    if (upper == "DEFAULT") {
        selectDefaultCalibrationProfile();
        logEvent("BLE", "profile_default");
        return;
    }

    int requestedIndex = value.toInt();
    if (requestedIndex > 0) {
        if (selectCalibrationProfile((uint8_t)(requestedIndex - 1))) {
            char payload[64];
            snprintf(payload, sizeof(payload), "{\"profile_index\":%d}", requestedIndex);
            logPacket("BLE", payload);
        } else {
            char payload[80];
            snprintf(payload, sizeof(payload), "{\"profile_index\":%d,\"ignored\":true}", requestedIndex);
            logPacket("BLE", payload);
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
            char payload[96];
            snprintf(payload, sizeof(payload), "{\"profile\":\"%s\"}", profile->name);
            logPacket("BLE", payload);
            return;
        }
    }

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"profile\":\"%s\",\"ignored\":true}", value.c_str());
    logPacket("BLE", payload);
}

static void applyProfileCommand(const String& valueRaw) {
    String value = valueRaw;
    value.trim();
    if (value.startsWith("SET_DEFAULT;id=")) {
        uint32_t id = (uint32_t)value.substring(14).toInt();
        setProfileDefaultById(id);
    } else if (value.startsWith("SELECT;id=")) {
        uint32_t id = (uint32_t)value.substring(10).toInt();
        selectCalibrationProfileById(id);
    } else if (value.startsWith("RENAME;id=")) {
        int namePos = value.indexOf(";name=");
        uint32_t id = (uint32_t)value.substring(10, namePos).toInt();
        String newName = value.substring(namePos + 6);
        renameCalibrationProfileById(id, newName.c_str());
    } else if (value.startsWith("DELETE;id=")) {
        uint32_t id = (uint32_t)value.substring(10).toInt();
        deleteCalibrationProfileById(id);
    } else if (value == "CLEAR_ALL") {
        clearCalibrationProfiles();
    }
}

static bool extractJsonStringField(const String& payload, const char* key, String& out) {
    String pattern = String("\"") + key + "\"";
    int keyPos = payload.indexOf(pattern);
    if (keyPos < 0) return false;
    int colonPos = payload.indexOf(':', keyPos + pattern.length());
    if (colonPos < 0) return false;
    int firstQuote = payload.indexOf('"', colonPos + 1);
    if (firstQuote < 0) return false;
    int secondQuote = payload.indexOf('"', firstQuote + 1);
    if (secondQuote < 0) return false;
    out = payload.substring(firstQuote + 1, secondQuote);
    return true;
}

static bool extractJsonIntField(const String& payload, const char* key, uint32_t& out) {
    String pattern = String("\"") + key + "\"";
    int keyPos = payload.indexOf(pattern);
    if (keyPos < 0) return false;
    int colonPos = payload.indexOf(':', keyPos + pattern.length());
    if (colonPos < 0) return false;
    int start = colonPos + 1;
    while (start < (int)payload.length() && isspace((unsigned char)payload[start])) start++;
    int end = start;
    while (end < (int)payload.length() && isdigit((unsigned char)payload[end])) end++;
    if (end <= start) return false;
    out = (uint32_t)payload.substring(start, end).toInt();
    return true;
}

static void parseAndApplyBleCommand(const String &payloadRaw) {
    String payload = payloadRaw;
    payload.trim();
    if (payload.length() == 0) return;

    if (payload.startsWith("{")) {
        String cmd;
        uint32_t seq = 0u;
        bool hasSeq = extractJsonIntField(payload, "seq", seq);
        bool hasCmd = extractJsonStringField(payload, "cmd", cmd);
        cmd.toUpperCase();
        bool ok = false;
        const char* error = nullptr;

        if (hasCmd) {
            if (cmd == "GET_PROFILES") {
                pendingProfileListSend = true;
                ok = true;
            } else if (cmd == "CALIBRATE_CANCEL") {
                requestCalibrationCancel();
                ok = true;
            } else if (cmd == "CALIBRATE_START") {
                String profileName = "Profile";
                String slotValue;
                extractJsonStringField(payload, "slot", slotValue);
                extractJsonStringField(payload, "name", profileName);
                if (slotValue == "auto") {
                    if (getProfileCount() >= 8) {
                        error = "PROFILE_CREATE_FAILED";
                    } else {
                        setPendingCalibrationName(profileName.c_str());
                        requestCalibrationStart();
                        ok = true;
                    }
                } else {
                    error = "INVALID_SLOT";
                }
            } else if (cmd == "PROFILE_SELECT") {
                uint32_t id = 0u;
                if (extractJsonIntField(payload, "id", id)) {
                    ok = selectCalibrationProfileById(id);
                    if (!ok) error = "PROFILE_NOT_FOUND";
                } else {
                    error = "BAD_REQUEST";
                }
            } else if (cmd == "PROFILE_SET_DEFAULT") {
                uint32_t id = 0u;
                if (extractJsonIntField(payload, "id", id)) {
                    ok = getProfileById(id) != nullptr;
                    if (ok) {
                        setProfileDefaultById(id);
                    } else {
                        error = "PROFILE_NOT_FOUND";
                    }
                } else {
                    error = "BAD_REQUEST";
                }
            } else if (cmd == "PROFILE_RENAME") {
                uint32_t id = 0u;
                String name;
                if (extractJsonIntField(payload, "id", id) && extractJsonStringField(payload, "name", name)) {
                    ok = renameCalibrationProfileById(id, name.c_str());
                    if (!ok) error = "PROFILE_NOT_FOUND";
                } else {
                    error = "BAD_REQUEST";
                }
            } else if (cmd == "PROFILE_DELETE") {
                uint32_t id = 0u;
                if (extractJsonIntField(payload, "id", id)) {
                    ok = deleteCalibrationProfileById(id);
                    if (!ok) error = "PROFILE_NOT_FOUND";
                } else {
                    error = "BAD_REQUEST";
                }
            } else if (cmd == "PROFILE_CLEAR_ALL") {
                clearCalibrationProfiles();
                ok = true;
            } else if (cmd == "GET_DEVICE_INFO") {
                sendDeviceInfoPacket();
                ok = true;
            } else if (cmd == "GET_THERAPY_PLAN") {
                if (therapyIsRunning()) {
                    sendTherapyPlanPacket();
                } else {
                    forceTelemetrySync = true;
                }
                ok = true;
            } else if (cmd == "STOP_THERAPY" || cmd == "THERAPY_STOP" ||
                       cmd == "STOP THERAPY" || cmd == "STOP") {
                stopTherapyFromBle();
                ok = true;
            } else if (cmd == "FACTORY_RESET") {
                pendingFactoryReset = true;
                ok = true;
            } else {
                error = "UNKNOWN_CMD";
            }

            if (hasSeq) {
                sendCommandAck(seq, cmd.c_str(), ok, error);
            }
        }
        return;
    }

    String requestedMode = "";
    String cmdName = "";
    String cmdParams[4];
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
            } else if (key == "CMD") {
                cmdName = value;
            } else if (key == "POSTURE_TIMING") {
                applyTrainingTiming(value);
            } else if (key == "THERAPY_DURATION_MIN") {
                applyTherapyDurationMinutes(value);
            } else if (key == "THERAPY_INTENSITY") {
                applyTherapyIntensity(value);
            } else if (key == "DIFFICULTY_DEG") {
                applyDifficultyDegrees(value);
            } else if (key == "PROFILE") {
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

    if (cmdName.length() > 0) {
        cmdName.trim();
        cmdName.toUpperCase();
        if (cmdName == "GET_PROFILES") {
            pendingProfileListSend = true;
        } else if (cmdName == "CALIBRATE_CANCEL") {
            requestCalibrationCancel();
        } else if (cmdName == "CALIBRATE_START") {
            String profileName = "Profile";
            bool autoSlot = false;
            start = 0;
            while (start < payloadLen) {
                int end = payload.indexOf(';', start);
                if (end < 0) end = payload.length();
                String token = payload.substring(start, end);
                token.trim();
                int sep = token.indexOf('=');
                if (sep > 0) {
                    String key = token.substring(0, sep);
                    String value = token.substring(sep + 1);
                    key.trim();
                    key.toUpperCase();
                    value.trim();
                    if (key == "slot" && value == "auto") autoSlot = true;
                    if (key == "name" && value.length() > 0) profileName = value;
                }
                start = end + 1;
            }
            if (autoSlot) {
                if (getProfileCount() < 8) {
                    setPendingCalibrationName(profileName.c_str());
                    requestCalibrationStart();
                }
            }
        } else if (cmdName == "GET_THERAPY_PLAN") {
            if (therapyIsRunning()) {
                sendTherapyPlanPacket();
            } else {
                forceTelemetrySync = true;
            }
        } else if (cmdName == "STOP_THERAPY" || cmdName == "THERAPY_STOP" ||
                   cmdName == "STOP THERAPY" || cmdName == "STOP") {
            stopTherapyFromBle();
        } else {
            String cmdValue = cmdName;
            if (payload.indexOf("PROFILE_SELECT;") >= 0 || payload.startsWith("PROFILE_SELECT")) {
                String idStr = payload.substring(payload.indexOf("id=") + 3);
                selectCalibrationProfileById((uint32_t)idStr.toInt());
            } else if (payload.startsWith("PROFILE_SET_DEFAULT")) {
                String idStr = payload.substring(payload.indexOf("id=") + 3);
                setProfileDefaultById((uint32_t)idStr.toInt());
            } else if (payload.startsWith("PROFILE_RENAME")) {
                int idPos = payload.indexOf("id=");
                int namePos = payload.indexOf(";name=");
                if (idPos >= 0 && namePos > idPos) {
                    uint32_t id = (uint32_t)payload.substring(idPos + 3, namePos).toInt();
                    String newName = payload.substring(namePos + 6);
                    renameCalibrationProfileById(id, newName.c_str());
                }
            } else if (payload.startsWith("PROFILE_DELETE")) {
                String idStr = payload.substring(payload.indexOf("id=") + 3);
                deleteCalibrationProfileById((uint32_t)idStr.toInt());
            } else if (payload.startsWith("PROFILE_CLEAR_ALL")) {
                clearCalibrationProfiles();
            }
        }
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
    rttPrintBlePacket("RX", payload.c_str());
#endif
    parseAndApplyBleCommand(payload);
}

void bluetoothSetup() {
    rtt.print("[BLE EVT] init ");
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
        rttPrintBlePacket("TX", "{}");

        pCharacteristic = &gCharacteristic;
        bleInitialized = true;
    }

    startAdvertising();
}

void bluetoothLoop() {
    if (!pCharacteristic) return;

    if (pendingProfileListSend && connected) {
        pendingProfileListSend = false;
        sendProfileList();
    }

    if (pendingDfuEnter) {
        pendingDfuEnter = false;
        rtt.println("DFU: entering OTA bootloader");
        delay(50);
        enterOTADfu();
        return;
    }

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

    // Check if telemetry state changed to set forceTelemetrySync
    char subModeStr[16];
    if (currentMode == MODE_TRAINING) {
        if (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) {
            strcpy(subModeStr, "INSTANT");
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
    const uint32_t profileId = activeProfile ? activeProfile->id : 0u;
    const char *profileName = activeProfile ? activeProfile->name : "DEFAULT";

    const char *modeString = currentModeText();

    updateBatteryReading(now);

    const bool runningNow = therapyIsRunning();

    bool telemetryChanged = !telemetryCacheValid ||
        strcmp(lastMode, modeString) != 0 ||
        strcmp(lastSubMode, subModeStr) != 0 ||
        strcmp(lastProfile, profileName) != 0 ||
        lastProfileId != profileId ||
        lastBatteryPercentage != batteryPercentage ||
        lastRunning != runningNow;

    if (telemetryChanged) {
        forceTelemetrySync = true;
    }

    if (connected) {
        if (runningNow) {
            const uint32_t sid = therapyGetSessionId();
            if (!therapyPlanSentForSession || lastTherapyPlanSessionId != sid) {
                sendTherapyPlanPacket();
                therapyPlanSentForSession = true;
                lastTherapyPlanSessionId = sid;
            }
        } else {
            therapyPlanSentForSession = false;
            lastTherapyPlanSessionId = 0;
        }

        const bool canSendBle =
            connected &&
            pCharacteristic != nullptr;

        if (canSendBle) {
            // 1. L: Always send live posture packet every 150 ms
            if (forceLiveSync ||
                lastLiveSendMs == 0 ||
                ((now - lastLiveSendMs) >= LIVE_PACKET_INTERVAL_MS)) {
                forceLiveSync = false;
                lastLiveSendMs = now;
                sendLivePacket();
            }

            // 2. T: Send training/status telemetry every 1000 ms in TRAINING mode, or on force sync
            if ((currentMode == MODE_TRAINING &&
                 (forceTelemetrySync ||
                  lastTrainingTelemetrySendMs == 0 ||
                  ((now - lastTrainingTelemetrySendMs) >= TRAINING_TELEMETRY_INTERVAL_MS))) ||
                (forceTelemetrySync && currentMode != MODE_TRAINING)) {
                forceTelemetrySync = false;
                lastTrainingTelemetrySendMs = now;
                sendStatusTelemetry("loop", "training_1s");
            }

            // 3. TL: Send therapy live progress every 1000 ms in THERAPY mode
            if (currentMode == MODE_THERAPY &&
                therapyIsRunning() &&
                (lastTherapyLiveSendMs == 0 ||
                 ((now - lastTherapyLiveSendMs) >= THERAPY_LIVE_PACKET_INTERVAL_MS))) {
                lastTherapyLiveSendMs = now;
                sendTherapyLivePacket();
            }
        }
    }

    if (pendingFactoryReset) {
        pendingFactoryReset = false;
        storageFactoryReset();
        saveBlePairMarker(false);
        NVIC_SystemReset();
        return;
    }

    // RTT mirror of the live packet stream. Keep this aligned with what the
    // app would receive over BLE instead of the old free-form STATUS output.
#if ALIGN_RTT_STATUS_LOG
    static unsigned long lastStatusMs = 0;
    if (!isCalibrating() && (now - lastStatusMs) >= ALIGN_RTT_STATUS_INTERVAL_MS) {
        lastStatusMs = now;
        char statusBuffer[192];
        snprintf(statusBuffer, sizeof(statusBuffer),
                 "{\"t\":\"S\",\"mode\":\"%s\",\"profile\":\"%s\",\"profiles\":%u,\"angle\":%.1f,\"dir\":\"%s\",\"posture\":\"%s\",\"bad\":%s,\"battery_mv\":%u,\"battery_pct\":%u,\"steps\":%lu}",
                 modeString,
                 profileName,
                 (unsigned)getProfileCount(),
                 currentAngle,
                 directionText,
                 postureText,
                 isBadPosture ? "true" : "false",
                 batteryMillivolts,
                 batteryPercentage,
                 (unsigned long)getDeviceStepCount());
        rttPrintBlePacket("TX", statusBuffer);
    }
#endif

}

void notifyCalibrationStatus(const char* calibResult, const char* complete) {
    if (!pCharacteristic || !connected) {
        return;
    }

    const bool calibrating = isCalibrating();
    const uint32_t elapsed = getCalibrationElapsedMs();
    const uint32_t total = getCalibrationTotalMs();
    const char* phase = getCalibrationPhase();

    char safeResult[24];
    if (!calibResult || calibResult[0] == '\0') {
        safeResult[0] = '\0';
    } else {
        strncpy(safeResult, calibResult, sizeof(safeResult) - 1);
        safeResult[sizeof(safeResult) - 1] = '\0';
    }

    char safeComplete[16];
    if (!complete || complete[0] == '\0') {
        safeComplete[0] = '\0';
    } else {
        strncpy(safeComplete, complete, sizeof(safeComplete) - 1);
        safeComplete[sizeof(safeComplete) - 1] = '\0';
    }

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"C\",\"isCalibrating\":%s,\"c_phase\":\"%s\",\"calibResult\":\"%s\",\"complete\":\"%s\",\"c_elap\":%lu,\"c_tot\":%lu}",
             calibrating ? "true" : "false",
             phase ? phase : "IDLE",
             safeResult,
             safeComplete,
             (unsigned long)elapsed,
             (unsigned long)total);
    sendBlePacket(payload);
}

void notifyDfuStatus(const char* status) {
    if (!pCharacteristic || !connected) {
        return;
    }

    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"D\",\"status\":\"%s\"}",
             (status && status[0] != '\0') ? status : "unknown");
    sendBlePacket(payload);
}

void notifyDeviceInfo() {
    if (!pCharacteristic || !connected) {
        return;
    }

    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"V\",\"device_model\":\"%s\",\"hw_version\":\"%s\",\"fw_version\":\"%s\",\"fw_build_date\":\"%s\"}",
             DEVICE_MODEL,
             HW_VERSION,
             FW_VERSION,
             FW_BUILD_DATE);
    sendBlePacket(payload);
}

uint8_t bluetoothGetBatteryPercentage() {
    return batteryPercentage;
}

bool bluetoothIsMotorActive() {
    return motorIsActive();
}

void bluetoothStartAdvertising() {
    rtt.println("[BLE EVT] start advertising");
    startAdvertising();
}

void bluetoothStopAdvertising() {
    rtt.println("[BLE EVT] stop advertising");
    Bluefruit.Advertising.stop();
}

bool bluetoothIsConnected() {
    return connected;
}

void bluetoothUnlockForPairing() {
    rtt.println("[BLE EVT] unlock pairing - clearing bonds");

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
