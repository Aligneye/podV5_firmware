#include "bluetooth.h"
#include "calibration.h"
#include "therapy.h"
#include "button.h"
#include "training.h"
#include "device_time.h"
#include "session_stats.h"
#include <bluefruit.h>

int therapyIntensityLevel = 2; // Default to Mid (2)

extern RTTStream rtt;

static bool connected = false;
static bool bleInitialized = false;

static BLEService gService(BLE_SERVICE_UUID);
static BLECharacteristic gCharacteristic(BLE_CHARACTERISTIC_UUID);
static BLECharacteristic *pCharacteristic = nullptr;

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
    (void)conn_handle;
    connected = true;
    rtt.println("BLE: Connected");
}

static void onBleDisconnect(uint16_t conn_handle, uint8_t reason) {
    (void)conn_handle;
    (void)reason;
    connected = false;
    rtt.println("BLE: Disconnected");
    startAdvertising();
}

static void applyTrainingTiming(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

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
}

static void applyTherapyDurationMinutes(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    int mins = value.toInt();
    if (mins <= 0) return;

    if (mins == 10) {
        therapySubModeIndex = 0;
    } else if (mins == 20) {
        therapySubModeIndex = 1;
    } else if (mins == 30) {
        therapySubModeIndex = 2;
    } else {
        therapySubModeIndex = 0; // default 10 min
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

    if (value == "TRACKING") {
        deviceOn = true;
        trainingSubModeIndex = TrainingAlertStyle::NoAlerts; // No alerts (equivalent to tracking)
        setDeviceMode(MODE_TRAINING);
        rtt.println("BLE CMD: MODE=TRACKING");
    } else if (value == "TRAINING" || value == "POSTURE") {
        deviceOn = true;
        if (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) {
            trainingSubModeIndex = TrainingAlertStyle::Instant; // Default back to Instant if it was tracking
        }
        setDeviceMode(MODE_TRAINING);
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

    rtt.print("BLE RX CMD: ");
    rtt.println(payload);
    parseAndApplyBleCommand(payload);
}

void bluetoothSetup() {
    rtt.print("Initializing BLE as: ");
    rtt.println(BLE_DEVICE_NAME);

    if (!bleInitialized) {
        Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
        Bluefruit.begin(1, 0);
        Bluefruit.autoConnLed(false); // Disable auto LED blinking on PIN_LED1/PIN_LED2 (P0.17/P0.19)
        Bluefruit.setName(BLE_DEVICE_NAME);
        Bluefruit.setTxPower(4); // dBm
        Bluefruit.Periph.setConnectCallback(onBleConnect);
        Bluefruit.Periph.setDisconnectCallback(onBleDisconnect);

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

    static unsigned long last = 0;
    unsigned long now = millis();
    unsigned long interval = isCalibrating() ? 150UL : 500UL;
    if (now - last < interval) return;
    last = now;

    // Calculate individual angles
    float ang_x = atan2(rawX, sqrt(rawY * rawY + rawZ * rawZ)) * 180.0 / PI;
    float ang_y = atan2(rawY, sqrt(rawX * rawX + rawZ * rawZ)) * 180.0 / PI;
    float ang_z = atan2(sqrt(rawX * rawX + rawY * rawY), rawZ) * 180.0 / PI;

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

    // JSON Construction
    char jsonBuffer[512];
    int offset = 0;

    bool calibrating = isCalibrating();
    unsigned long calibElapsedMs = getCalibrationElapsedMs();
    unsigned long calibTotalMs = getCalibrationTotalMs();
    const char *calibResult = getCalibrationResult();

    const char *modeString = "TRACKING";
    if (currentMode == MODE_TRAINING) {
        modeString = (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) ? "TRACKING" : "TRAINING";
    } else if (currentMode == MODE_THERAPY) {
        modeString = "THERAPY";
    } else if (currentMode == MODE_OFF) {
        modeString = "OFF";
    }

    offset += snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset,
        "{\"mode\":\"%s\",\"sub_mode\":\"%s\",\"angle\":%.2f,"
        "\"raw_x_g\":%.2f,\"raw_y_g\":%.2f,\"raw_z_g\":%.2f,"
        "\"angle_x\":%.1f,\"angle_y\":%.1f,\"angle_z\":%.1f,"
        "\"cal_y\":%.2f,\"cal_z\":%.2f,"
        "\"is_calibrating\":%s,\"c_phase\":\"%s\",\"c_elap\":%lu,\"c_tot\":%lu,",
        modeString,
        subModeStr, currentAngle,
        rawX, rawY, rawZ,
        ang_x, ang_y, ang_z,
        Y_ORIGIN, Z_ORIGIN,
        calibrating ? "true" : "false", getCalibrationPhase(), calibElapsedMs, calibTotalMs
    );

    if (calibResult[0] != '\0' && offset < sizeof(jsonBuffer)) {
        offset += snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset,
            "\"calibration_result\":\"%s\",", calibResult);
    }

    // Dummy values for battery since pin is not defined
    float dummyBatteryVolt = 3.82f;
    int dummyBatteryPct = 80;

    if (offset < sizeof(jsonBuffer)) {
        offset += snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset,
            "\"posture\":\"%s\",\"is_bad_posture\":%s,\"battery_voltage\":%.2f,\"battery_percentage\":%d",
            postureText, isBadPosture ? "true" : "false", dummyBatteryVolt, dummyBatteryPct
        );
    }

    if (offset < sizeof(jsonBuffer)) {
        offset += snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset,
            ",\"difficulty_deg\":%d", (int)kBadPostureDeg
        );
    }

    if (isTrainingActive() && offset < sizeof(jsonBuffer)) {
        offset += snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset,
            ",\"s_id\":%lu,\"s_elap\":%lu,\"s_start\":%lu,\"s_bad\":%lu",
            (unsigned long)getTrainingSessionNumber(),
            (unsigned long)getTrainingSessionDurationSec(),
            (unsigned long)getActiveTrainingStartEpoch(),
            (unsigned long)getTrainingSessionBadPostureCount()
        );
    } else if (isTherapyActive() && offset < sizeof(jsonBuffer)) {
        offset += snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset,
            ",\"s_id\":%lu,\"s_elap\":%lu,\"s_start\":%lu,\"s_bad\":0",
            (unsigned long)getTherapySessionNumber(),
            (unsigned long)getTherapySessionDurationSec(),
            (unsigned long)getActiveTherapyStartEpoch()
        );
    }

    if (currentMode == MODE_THERAPY && offset < sizeof(jsonBuffer)) {
        unsigned long therapyRemainingSec = (therapyGetRemainingMs() + 999UL) / 1000UL;
        unsigned long therapyElapsedSec = therapyGetElapsedMs() / 1000UL;
        char seqStr[64] = "";
        int seqOffset = 0;
        uint8_t seq[20];
        int seqLen = getTherapyPatternSequence(seq, 20);
        for (int i = 0; i < seqLen; i++) {
            seqOffset += snprintf(seqStr + seqOffset, sizeof(seqStr) - seqOffset,
                                  (i == 0) ? "%d" : ",%d", seq[i]);
        }
        offset += snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset,
            ",\"t_patt\":\"%s\",\"t_next\":\"%s\",\"t_elap\":%lu,\"t_rem\":%lu,\"t_lvl\":%d,\"t_seq\":\"%s\",\"t_cur\":%d,\"t_total\":%d",
            therapyGetCurrentPatternName(), therapyGetNextPatternName(),
            therapyElapsedSec, therapyRemainingSec,
            therapyIntensityLevel,
            seqStr,
            currentPatternIndex,
            getTherapyTotalPatternCount()
        );
    }

    if (offset < sizeof(jsonBuffer)) {
        snprintf(jsonBuffer + offset, sizeof(jsonBuffer) - offset, "}");
    } else {
        jsonBuffer[sizeof(jsonBuffer) - 2] = '}';
        jsonBuffer[sizeof(jsonBuffer) - 1] = '\0';
    }

    // Send if connected
    if (connected) {
        pCharacteristic->write(jsonBuffer);
        pCharacteristic->notify(jsonBuffer);
    }

    // Also write JSON to RTT (suppressed during active calibration to avoid spamming logs)
    if (!isCalibrating()) {
        rtt.println(jsonBuffer);
    }
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

void bluetoothRequestCalibrationStart() {
    calibrationRequestStart();
}

void notifyNewSessionStored() {
    // Placeholder
}
