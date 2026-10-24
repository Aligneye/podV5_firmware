#include "bluetooth.h"
#include "calibration.h"
#include "therapy.h"
#include "button.h"
#include "training.h"
#include "motor.h"
#include "storage.h"
#include "device_time.h"
#include "session_stats.h"
#include "session_log.h"
#include "BatteryMonitor.h"
#include "version.h"
#include "rtt_debugger.h"
#include <cstddef>
#include <cstring>
#include <bluefruit.h>
#include <ble_hci.h>
#include <utility/bonding.h>
#if __has_include(<InternalFileSystem.h>)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#define BLE_PAIR_STORE_HAS_FS 1
#else
#define BLE_PAIR_STORE_HAS_FS 0
#endif

int therapyIntensityLevel = 2; // Default to Mid (2)

static bool connected = false;
static bool bleInitialized = false;
static uint16_t currentConnHandle = BLE_CONN_HANDLE_INVALID;
static volatile bool applicationAuthorized = false;
static bool connectionHapticPending = false;
static bool connectionHapticPlayed = false;
static bool sleepShutdownRequested = false;
static bool forceTelemetrySync = false;
static bool forceLiveSync = false;
static unsigned long lastLiveSendMs = 0;
static unsigned long lastTrainingTelemetrySendMs = 0;
static unsigned long lastTherapyLiveSendMs = 0;
static unsigned long lastStatusSendMs = 0;
static volatile bool notificationsEnabled = false;
static bool applicationTxReadyLogged = false;

static bool telemetryCacheValid = false;
static char lastMode[12] = "";
static char lastSubMode[16] = "";
static char lastProfile[32] = "";
static uint32_t lastProfileId = 0u;
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

// ── Session sync FSM ─────────────────────────────────────────────────────
// Streams stored sessions to the app after a FETCH_SESSIONS command.
// One packet per loop tick, advanced only when notify() succeeds; nothing
// is marked sent on flash until the app's final ACK_SESSIONS arrives.
enum class SyncState : uint8_t { Idle, Hdr, Stream, End, WaitAck, Retx };

struct SyncEntry {
    StoredSession s;
    uint16_t seqBase;   // seq of this entry's summary packet
    uint8_t  evCount;   // event items (posture pairs / therapy patterns)
    uint8_t  evChunks;  // SEV packets that follow the summary
};

static SyncState syncState = SyncState::Idle;
static SyncEntry syncWindow[SYNC_MAX_WINDOW];
static uint8_t  syncCount = 0;
static uint8_t  syncEntryIdx = 0;
static uint8_t  syncChunkIdx = 0;       // 0 = summary, 1..evChunks = SEV chunk
static uint16_t syncSeq = 0;
static uint32_t syncXferId = 0;
static uint16_t syncPairsPerChunk = 9;  // fixed per transfer from the MTU
static uint16_t syncTherapyPerChunk = 30;
static unsigned long syncAckWaitStartMs = 0;
static bool syncFastParamsActive = false;
static volatile bool syncFetchRequested = false;
static volatile bool syncAckReceived = false;
static volatile bool syncAckOk = false;
static uint16_t syncRetxList[16];
static volatile uint8_t syncRetxCount = 0;
static uint8_t syncRetxIdx = 0;
static volatile bool sessAvailPending = false;

// Event cache for the entry currently being streamed / retransmitted.
static PostureEventReadResult syncPostureEv;
static TherapyEventReadResult syncTherapyEv;
static int syncEvLoadedFor = -1;

static void syncReset();
static void onBleRawEvent(ble_evt_t* evt);
static void onBleCccdWrite(uint16_t conn_handle, BLECharacteristic* chr, uint16_t value);

static constexpr uint32_t LIVE_PACKET_INTERVAL_MS = 500UL;
static constexpr uint32_t TRAINING_TELEMETRY_INTERVAL_MS = 1000UL;
static constexpr uint32_t THERAPY_LIVE_PACKET_INTERVAL_MS = 1000UL;
static constexpr uint32_t STATUS_PACKET_INTERVAL_MS = 5000UL;
static constexpr uint32_t BATTERY_BLINK_PERIOD_MS = 1000UL;
static constexpr uint8_t BATTERY_BLINK_COUNT = 5;
static constexpr uint32_t UNPAIRED_RED_BLINK_PERIOD_MS = 160UL;
static constexpr uint32_t CONNECTION_HAPTIC_DELAY_MS = 800UL;
static constexpr uint16_t CONNECTION_HAPTIC_MS = 150U;
static constexpr uint32_t REMOTE_UNPAIR_ACK_TIMEOUT_MS = 3000UL;
static constexpr uint32_t REMOTE_UNPAIR_DISCONNECT_DELAY_MS = 400UL;
static const char* BLE_PAIR_MARKER_PATH = "/ble_pair.dat"; // Removed during migration.

// -------------------------------------------------------------------------
// Single-owner pairing lock
// -------------------------------------------------------------------------
//
// A BLE connection is not an authorization decision.  The pod authorizes the
// app only after the connecting peer matches the stored owner identity and the
// SoftDevice reports an encrypted link.  New bonds are accepted only during a
// short window opened by the physical button.
enum class PairingLockState : uint8_t {
    NoOwnerClosed,
    PairingOpen,
    ClaimCommitting,
    OwnerLocked,
    OwnerSecuring,
    OwnerAuthorized,
    Revoking
};

static volatile PairingLockState pairingLockState = PairingLockState::NoOwnerClosed;

static constexpr uint32_t BLE_OWNER_MAGIC = 0x424F574EUL; // "BOWN"
static constexpr uint16_t BLE_OWNER_VERSION = 1u;
static constexpr uint16_t BLE_OWNER_VALID = 0x0001u;
static constexpr uint16_t BLE_OWNER_REVOCATION_PENDING = 0x0002u;
static constexpr uint8_t BLE_ADV_STATUS_VERSION = 1u;
static constexpr uint8_t BLE_ADV_STATUS_OWNER_BONDED = 0x01u;
static constexpr uint8_t BLE_ADV_STATUS_PAIRING_OPEN = 0x02u;
static const char* BLE_OWNER_PATH = "/ble_owner.dat";
static const char* BLE_OWNER_TMP_PATH = "/ble_owner.tmp";

// Store only a stable reference to Bluefruit's bond.  The LTK and IRK remain
// in Bluefruit's bond file; duplicating encryption keys creates two sources of
// truth that can drift apart after a reset.
struct BleOwnerRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint8_t addressType;
    uint8_t address[BLE_GAP_ADDR_LEN];
    uint8_t reserved;
    uint32_t bondFingerprint;
    uint32_t crc;
};

static BleOwnerRecord ownerRecord = {};
static bond_keys_t ownerBond = {};
static bool ownerRecordValid = false;
static bool connectedPeerMatchesOwner = false;
static bool linkSecured = false;
static unsigned long pairingWindowStartedMs = 0;
static unsigned long securityDeadlineStartedMs = 0;
static unsigned long claimStartedMs = 0;
static unsigned long lastClaimLoadAttemptMs = 0;

// BLE callbacks run on Bluefruit's callback task.  They only copy compact
// event state here; filesystem work and advertising transitions are deferred
// to bluetoothLoop(), which runs on the main firmware task.
static volatile bool pendingAdvertisingRestart = false;
static volatile bool pendingUnlockFinalize = false;
static volatile bool pendingPairingWindowClose = false;
static volatile bool pendingUnauthorizedDisconnect = false;
static volatile bool pendingSecuredEvent = false;
static volatile uint16_t pendingSecuredHandle = BLE_CONN_HANDLE_INVALID;
static volatile bool pendingAuthEvent = false;
static volatile uint8_t pendingAuthStatus = BLE_GAP_SEC_STATUS_UNSPECIFIED;
static volatile bool pendingAuthBonded = false;
static ble_gap_addr_t pendingClaimIdentity = {};
static bool claimAuthSucceeded = false;
static bool claimLinkSecured = false;
static bool preservePairingWindowOnCleanup = false;
static bool closePairingWindowAfterCleanup = false;

// A successful UNPAIR command is acknowledged before ownership is revoked.
// The BLE callback only queues the request; notification, delay, flash work,
// disconnect, and advertising transitions all run from bluetoothLoop().
enum class RemoteUnpairState : uint8_t {
    Idle,
    AckPending,
    AckSentWaiting
};

static volatile RemoteUnpairState remoteUnpairState = RemoteUnpairState::Idle;
static volatile uint32_t remoteUnpairSeq = 0u;
static volatile unsigned long remoteUnpairQueuedMs = 0UL;
static unsigned long remoteUnpairAckSentMs = 0UL;

// We use a small SoftDevice advertiser instead of Bluefruit.Advertising.start()
// because this version of Bluefruit hardcodes BLE_GAP_ADV_FP_ANY.  Owning the
// advertising parameters here lets locked mode use the controller whitelist
// without patching an installed PlatformIO package.
static uint8_t pairingAdvHandle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static bool pairingAdvertisingRunning = false;
static ble_gap_adv_data_t pairingAdvData = {};

static void startAdvertising();
static void stopPairingAdvertising();
static void processPairingLock(unsigned long now);
static bool processRemoteUnpair(unsigned long now);
static void beginPairingUnlock(const char* source);

static void updateBatteryReading(unsigned long now);

static bool bleApplicationTxAllowed(unsigned long now) {
    // Owner identity and encryption remain mandatory. Once authorized, use
    // the actual notification subscription as the readiness signal instead
    // of imposing a fixed delay on every bonded reconnect.
    if (!connected || !applicationAuthorized || !pCharacteristic) return false;
    if (!notificationsEnabled && !pCharacteristic->notifyEnabled()) return false;

    notificationsEnabled = true;
    if (!applicationTxReadyLogged) {
        applicationTxReadyLogged = true;
        forceTelemetrySync = true;
        forceLiveSync = true;
        lastLiveSendMs = 0;
        lastTrainingTelemetrySendMs = 0;
        lastTherapyLiveSendMs = 0;
        lastStatusSendMs = 0;
        rtt.printf("[BLEL] %lu APP_TX_OPEN h=%u\n", now, currentConnHandle);
    }
    return true;
}

static bool sendBlePacketInternal(const char* payload) {
    if (!pCharacteristic || !payload || !bleApplicationTxAllowed(millis())) {
        return false;
    }
    pCharacteristic->write(payload);
    const bool notified = pCharacteristic->notify(payload);
    if (!notified && connected) {
        // notify() fails for two very different reasons; tell them apart:
        //  - "no-cccd": phone hasn't (re)enabled notifications — nothing sent
        //  - "queue":   SoftDevice HVN buffers exhausted mid-packet — the
        //               packet went out TORN (leading chunks only)
        rtt.print("[BLE TX] NOTIFY-FAIL (");
        rtt.print(pCharacteristic->notifyEnabled() ? "queue" : "no-cccd");
        rtt.print(") len=");
        rtt.print(strlen(payload));
        rtt.print(" head=");
        char head[25];
        strncpy(head, payload, sizeof(head) - 1);
        head[sizeof(head) - 1] = '\0';
        rtt.println(head);
    }
    // RTT mirror moved to rtt_debugger.cpp.
    rttDebuggerPrintBlePacket("TX", payload);
    return notified;
}

static bool sendBlePacket(const char* payload) {
    return sendBlePacketInternal(payload);
}

static bool sendCommandAck(uint32_t seq, const char* cmd, bool ok,
                           const char* error = nullptr) {
    (void)error;
    if (!cmd || cmd[0] == '\0') return false;
    char payload[192];
    if (ok) {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"ACK\",\"seq\":%lu,\"cmd\":\"%s\",\"ok\":true}",
                 (unsigned long)seq, cmd);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"ACK\",\"seq\":%lu,\"cmd\":\"%s\",\"ok\":false}",
                 (unsigned long)seq, cmd);
    }
    return sendBlePacketInternal(payload);
}

static void getDeviceSerial(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    snprintf(out, outSize, "%08lX%08lX",
             (unsigned long)NRF_FICR->DEVICEID[1],
             (unsigned long)NRF_FICR->DEVICEID[0]);
}

static void sendDeviceInfoPacket() {
    if (!pCharacteristic || !connected) return;
    char serial[17];
    getDeviceSerial(serial, sizeof(serial));

    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"INFO\",\"device_name\":\"%s\",\"device_model\":\"%s\",\"hw\":\"%s\",\"fw\":\"%s\",\"fw_build_date\":\"%s\",\"serial\":\"%s\"}",
             DEVICE_NAME,
             DEVICE_MODEL,
             HW_VERSION,
             FW_VERSION,
             FW_BUILD_DATE,
             serial);
    sendBlePacket(payload);
}

static void sendTherapyPlanPacket() {
    if (!pCharacteristic || !connected) return;

    therapyEnsurePatternsInitialized();

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
             "{\"t\":\"TL\",\"sid\":%lu,\"idx\":%d,\"pid\":%d,\"elapsed\":%lu,\"remaining\":%lu,\"p_elapsed\":%lu,\"p_remaining\":%lu,\"intensity\":%d}",
             (unsigned long)therapyGetSessionId(),
             (int)therapyGetCurrentPatternIndex(),
             (int)therapyGetCurrentPatternId(),
             (unsigned long)therapyGetElapsedSeconds(),
             (unsigned long)therapyGetRemainingSeconds(),
             (unsigned long)therapyGetPatternElapsedSeconds(),
             (unsigned long)therapyGetPatternRemainingSeconds(),
             therapyIntensityLevel);
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

static void currentSubModeText(char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    if (currentMode == MODE_TRAINING) {
        if (trainingSubModeIndex == TrainingAlertStyle::NoAlerts) {
            snprintf(out, outSize, "NO_ALERTS");
        } else if (trainingSubModeIndex == TrainingAlertStyle::Instant) {
            snprintf(out, outSize, "INSTANT");
        } else {
            snprintf(out, outSize, "DELAYED");
        }
        return;
    }

    if (currentMode == MODE_THERAPY) {
        snprintf(out, outSize, "20 MIN");
        return;
    }

    snprintf(out, outSize, "IDLE");
}

static void sendLivePacket() {
    if (!pCharacteristic || !connected) return;
    if (isCalibrating()) return;

    updatePostureAngle();
    const char* postureStr = (currentAngle > kBadPostureDeg || currentAngle < -kBadPostureDeg)
        ? "BAD POSTURE"
        : "GOOD POSTURE";

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"L\",\"mode\":\"%s\",\"angle\":%.2f,\"difficulty_angle\":%.0f,\"posture\":\"%s\"}",
             currentModeText(),
             currentAngle,
             kBadPostureDeg,
             postureStr);

    sendBlePacket(payload);
}

// STATUS: periodic device health packet — battery percentage plus the latest
// raw accelerometer axes. Sent every STATUS_PACKET_INTERVAL_MS regardless of
// mode so the app always has a fresh battery + orientation reading.
static void sendStatusPacket() {
    if (!pCharacteristic || !connected) return;

    updatePostureAngle();  // refresh rawX/Y/Z from the sensor

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"STATUS\",\"battery\":%u,\"x\":%.4f,\"y\":%.4f,\"z\":%.4f}",
             (unsigned)batteryPercentage,
             rawX,
             rawY,
             rawZ);
    sendBlePacket(payload);
}

static void sendStatusTelemetry(const char* source = nullptr, const char* reason = nullptr) {
    (void)source;
    (void)reason;
    if (!pCharacteristic || !connected) return;
    if (isCalibrating()) return;
    if (currentMode != MODE_TRAINING) return;

    char subModeStr[16];
    currentSubModeText(subModeStr, sizeof(subModeStr));

    const OrientationProfile *activeProfile = getActiveProfile();
    const uint32_t profileId = activeProfile ? activeProfile->id : 0u;
    const char *profileName = activeProfile ? activeProfile->name : "DEFAULT";

    updatePostureAngle();
    const bool badNow = (currentAngle > kBadPostureDeg || currentAngle < -kBadPostureDeg);

    char telemetryBuffer[256];
    snprintf(
      telemetryBuffer,
      sizeof(telemetryBuffer),
      "{\"t\":\"T\",\"mode\":\"%s\",\"sub_mode\":\"%s\","
      "\"angle\":%.2f,"
      "\"difficulty_angle\":%.0f,"
      "\"posture\":\"%s\","
      "\"delay_ms\":%lu,"
      "\"alert_active\":%s,"
      "\"profile_id\":%lu,\"profile\":\"%s\"}",
      "TRAINING",
      subModeStr,
      currentAngle,
      kBadPostureDeg,
      badNow ? "BAD POSTURE" : "GOOD POSTURE",
      (unsigned long)trainingDelayedAlertMs,
      isTrainingMotorAlertActive ? "true" : "false",
      (unsigned long)profileId,
      profileName
    );
    sendBlePacket(telemetryBuffer);

    // Update state caches
    strncpy(lastMode, "TRAINING", sizeof(lastMode) - 1);
    lastMode[sizeof(lastMode) - 1] = '\0';
    strncpy(lastSubMode, subModeStr, sizeof(lastSubMode) - 1);
    lastSubMode[sizeof(lastSubMode) - 1] = '\0';
    strncpy(lastProfile, profileName, sizeof(lastProfile) - 1);
    lastProfile[sizeof(lastProfile) - 1] = '\0';
    lastProfileId = profileId;
    lastRunning = false;
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

// Compact link-setup trace. Connection intervals are in 1.25 ms units and
// supervision timeouts are in 10 ms units. Keep this callback lightweight:
// it runs from Bluefruit's BLE event path and is intended only for RTT capture.
static void onBleRawEvent(ble_evt_t* evt) {
    if (!evt) return;

    const unsigned long now = millis();
    const uint16_t handle = evt->evt.common_evt.conn_handle;

    switch (evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED: {
            const ble_gap_conn_params_t& p =
                evt->evt.gap_evt.params.connected.conn_params;
            rtt.printf("[BLEL] %lu CON h=%u i=%u lat=%u to=%u\n",
                       now, handle, p.max_conn_interval, p.slave_latency,
                       p.conn_sup_timeout);
            break;
        }

        case BLE_GAP_EVT_DISCONNECTED:
            rtt.printf("[BLEL] %lu DIS h=%u r=0x%02X\n", now, handle,
                       evt->evt.gap_evt.params.disconnected.reason);
            break;

        case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST: {
            const ble_gap_conn_params_t& p =
                evt->evt.gap_evt.params.conn_param_update_request.conn_params;
            rtt.printf("[BLEL] %lu CP_REQ h=%u i=%u-%u lat=%u to=%u\n",
                       now, handle, p.min_conn_interval, p.max_conn_interval,
                       p.slave_latency, p.conn_sup_timeout);
            break;
        }

        case BLE_GAP_EVT_CONN_PARAM_UPDATE: {
            const ble_gap_conn_params_t& p =
                evt->evt.gap_evt.params.conn_param_update.conn_params;
            rtt.printf("[BLEL] %lu CP_OK h=%u i=%u lat=%u to=%u\n",
                       now, handle, p.max_conn_interval, p.slave_latency,
                       p.conn_sup_timeout);
            break;
        }

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            rtt.printf("[BLEL] %lu SEC_REQ h=%u\n", now, handle);
            // Bluefruit's internal handler accepts this request before the
            // application event hook runs.  In locked states we therefore
            // fail closed by scheduling an immediate disconnect.  In normal
            // operation the controller whitelist prevents this path entirely.
            if (pairingLockState != PairingLockState::PairingOpen &&
                pairingLockState != PairingLockState::ClaimCommitting) {
                pendingUnauthorizedDisconnect = true;
            }
            break;

        case BLE_GAP_EVT_SEC_INFO_REQUEST:
            rtt.printf("[BLEL] %lu KEY_REQ h=%u\n", now, handle);
            break;

        case BLE_GAP_EVT_AUTH_STATUS: {
            const ble_gap_evt_auth_status_t& s =
                evt->evt.gap_evt.params.auth_status;
            rtt.printf("[BLEL] %lu AUTH h=%u st=0x%02X bond=%u lesc=%u src=%u\n",
                       now, handle, s.auth_status, s.bonded, s.lesc,
                       s.error_src);
            pendingAuthStatus = s.auth_status;
            pendingAuthBonded = s.bonded != 0;
            pendingAuthEvent = true;

            // Keep the address used for this connection.  bond_load_keys()
            // resolves an RPA with the newly stored IRK, so this works for
            // both private-address phones and public/static-address phones.
            BLEConnection* conn = Bluefruit.Connection(handle);
            if (conn) {
                pendingClaimIdentity = conn->getPeerAddr();
            }
            break;
        }

        case BLE_GAP_EVT_CONN_SEC_UPDATE: {
            const ble_gap_conn_sec_t& s =
                evt->evt.gap_evt.params.conn_sec_update.conn_sec;
            rtt.printf("[BLEL] %lu SEC_OK h=%u sm=%u lv=%u key=%u\n",
                       now, handle, s.sec_mode.sm, s.sec_mode.lv,
                       s.encr_key_size);
            if (s.sec_mode.sm != 0 && s.sec_mode.lv != 0) {
                pendingSecuredHandle = handle;
                pendingSecuredEvent = true;
            }
            break;
        }

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
            const ble_gap_phys_t& p =
                evt->evt.gap_evt.params.phy_update_request.peer_preferred_phys;
            rtt.printf("[BLEL] %lu PHY_REQ h=%u tx=%u rx=%u\n",
                       now, handle, p.tx_phys, p.rx_phys);
            break;
        }

        case BLE_GAP_EVT_PHY_UPDATE: {
            const ble_gap_evt_phy_update_t& p =
                evt->evt.gap_evt.params.phy_update;
            rtt.printf("[BLEL] %lu PHY_OK h=%u st=0x%02X tx=%u rx=%u\n",
                       now, handle, p.status, p.tx_phy, p.rx_phy);
            break;
        }

        case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST: {
            const ble_gap_data_length_params_t& p =
                evt->evt.gap_evt.params.data_length_update_request.peer_params;
            rtt.printf("[BLEL] %lu DL_REQ h=%u tx=%u rx=%u\n",
                       now, handle, p.max_tx_octets, p.max_rx_octets);
            break;
        }

        case BLE_GAP_EVT_DATA_LENGTH_UPDATE: {
            const ble_gap_data_length_params_t& p =
                evt->evt.gap_evt.params.data_length_update.effective_params;
            rtt.printf("[BLEL] %lu DL_OK h=%u tx=%u rx=%u\n",
                       now, handle, p.max_tx_octets, p.max_rx_octets);
            break;
        }

        case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST: {
            const uint16_t requested =
                evt->evt.gatts_evt.params.exchange_mtu_request.client_rx_mtu;
            BLEConnection* conn = Bluefruit.Connection(handle);
            const uint16_t active = conn ? conn->getMtu() : 0;
            rtt.printf("[BLEL] %lu MTU_REQ h=%u req=%u now=%u\n",
                       now, handle, requested, active);
            break;
        }

        case BLE_GATTC_EVT_EXCHANGE_MTU_RSP:
            rtt.printf("[BLEL] %lu MTU_RSP h=%u mtu=%u\n", now, handle,
                       evt->evt.gattc_evt.params.exchange_mtu_rsp.server_rx_mtu);
            break;

        case BLE_GAP_EVT_TIMEOUT:
            rtt.printf("[BLEL] %lu GAP_TO h=%u src=%u\n", now, handle,
                       evt->evt.gap_evt.params.timeout.src);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            rtt.printf("[BLEL] %lu GATTS_TO h=%u src=%u\n", now, handle,
                       evt->evt.gatts_evt.params.timeout.src);
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            rtt.printf("[BLEL] %lu GATTC_TO h=%u src=%u\n", now, handle,
                       evt->evt.gattc_evt.params.timeout.src);
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            rtt.printf("[BLEL] %lu SYS_ATTR h=%u\n", now, handle);
            break;

        default:
            break;
    }
}

static void onBleCccdWrite(uint16_t conn_handle, BLECharacteristic* chr,
                           uint16_t value) {
    (void)chr;
    if (conn_handle == currentConnHandle) {
        // Bit 0 enables notifications. This is the real application-transport
        // readiness event; it replaces the former unconditional 5 s delay.
        notificationsEnabled = (value & 0x0001u) != 0u;
        if (!notificationsEnabled) {
            applicationTxReadyLogged = false;
        }
    }
    rtt.printf("[BLEL] %lu CCCD h=%u v=0x%04X\n",
               millis(), conn_handle, value);
}

static void setRgbLedPwm(uint8_t red, uint8_t green, uint8_t blue) {
    analogWrite(PIN_LED_RED, 255 - red);
    analogWrite(PIN_LED_GREEN, 255 - green);
    analogWrite(PIN_LED_BLUE, 255 - blue);
}

static void turnRgbLedOff() {
    setRgbLedPwm(0, 0, 0);
}

static uint32_t pairingCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320UL : 0UL);
        }
    }
    return ~crc;
}

static uint32_t bondFingerprint(const bond_keys_t& bond) {
    // This value detects owner-record/bond-store mismatches.  It is not used
    // as an authentication secret; the LTK itself remains in Bluefruit's bond
    // store and is proven by the BLE encryption procedure.
    return pairingCrc32(reinterpret_cast<const uint8_t*>(&bond), sizeof(bond));
}

static ble_gap_addr_t ownerIdentityAddress(const BleOwnerRecord& record) {
    ble_gap_addr_t address = {};
    address.addr_type = record.addressType;
    memcpy(address.addr, record.address, BLE_GAP_ADDR_LEN);
    return address;
}

static void fillOwnerRecord(BleOwnerRecord& record,
                            const ble_gap_addr_t& identity,
                            const bond_keys_t& bond,
                            uint16_t flags) {
    memset(&record, 0, sizeof(record));
    record.magic = BLE_OWNER_MAGIC;
    record.version = BLE_OWNER_VERSION;
    record.flags = flags;
    record.addressType = identity.addr_type;
    memcpy(record.address, identity.addr, BLE_GAP_ADDR_LEN);
    record.bondFingerprint = bondFingerprint(bond);
    record.crc = pairingCrc32(
        reinterpret_cast<const uint8_t*>(&record), offsetof(BleOwnerRecord, crc));
}

static bool ownerRecordIsStructurallyValid(const BleOwnerRecord& record) {
    if (record.magic != BLE_OWNER_MAGIC ||
        record.version != BLE_OWNER_VERSION ||
        (record.flags & BLE_OWNER_VALID) == 0u) {
        return false;
    }

    const uint32_t expected = pairingCrc32(
        reinterpret_cast<const uint8_t*>(&record), offsetof(BleOwnerRecord, crc));
    return expected == record.crc;
}

static bool loadOwnerRecord(BleOwnerRecord& record) {
#if BLE_PAIR_STORE_HAS_FS
    InternalFS.begin();
    File file = InternalFS.open(BLE_OWNER_PATH, FILE_O_READ);
    if (!file) return false;
    const int count = file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record));
    file.close();
    return count == (int)sizeof(record) && ownerRecordIsStructurallyValid(record);
#else
    (void)record;
    return false;
#endif
}

static bool saveOwnerRecord(const BleOwnerRecord& requested) {
#if BLE_PAIR_STORE_HAS_FS
    BleOwnerRecord record = requested;
    record.crc = pairingCrc32(
        reinterpret_cast<const uint8_t*>(&record), offsetof(BleOwnerRecord, crc));

    InternalFS.begin();
    InternalFS.remove(BLE_OWNER_TMP_PATH);
    File file = InternalFS.open(BLE_OWNER_TMP_PATH, FILE_O_WRITE);
    if (!file) return false;

    file.seek(0);
    const size_t written = file.write(
        reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    file.truncate(sizeof(record));
    file.flush();
    file.close();
    if (written != sizeof(record)) {
        InternalFS.remove(BLE_OWNER_TMP_PATH);
        return false;
    }

    // LittleFS rename is atomic.  A reset leaves either the old complete
    // record or the new complete record, never a partially written owner.
    if (!InternalFS.rename(BLE_OWNER_TMP_PATH, BLE_OWNER_PATH)) {
        InternalFS.remove(BLE_OWNER_TMP_PATH);
        return false;
    }
    return true;
#else
    (void)requested;
    return false;
#endif
}

static void clearOwnerRecordFiles() {
#if BLE_PAIR_STORE_HAS_FS
    InternalFS.begin();
    InternalFS.remove(BLE_OWNER_TMP_PATH);
    InternalFS.remove(BLE_OWNER_PATH);
    // Delete the obsolete one-byte marker so an older firmware's UI state
    // cannot be mistaken for the new owner record after migration.
    InternalFS.remove(BLE_PAIR_MARKER_PATH);
#endif
}

static bool loadAndVerifyOwnerBond(const BleOwnerRecord& record,
                                   bond_keys_t& bond) {
    ble_gap_addr_t identity = ownerIdentityAddress(record);
    if (!bond_load_keys(BLE_GAP_ROLE_PERIPH, &identity, &bond)) {
        return false;
    }
    return record.bondFingerprint == bondFingerprint(bond);
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
    if (pairingLockState != PairingLockState::PairingOpen &&
        pairingLockState != PairingLockState::ClaimCommitting) {
        return false;
    }

    // A fast red blink makes the physically opened enrollment window visible
    // to the user.  It stops as soon as the new owner is safely committed.
    const bool on = ((now / UNPAIRED_RED_BLINK_PERIOD_MS) & 1u) == 0u;
    if (on) {
        setRgbLedPwm(255, 0, 0);
    } else {
        turnRgbLedOff();
    }
    return true;
}

static bool addressesEqual(const ble_gap_addr_t& left,
                           const ble_gap_addr_t& right) {
    return left.addr_type == right.addr_type &&
           memcmp(left.addr, right.addr, BLE_GAP_ADDR_LEN) == 0;
}

static bool peerMatchesStoredOwner(const ble_gap_addr_t& peer) {
    if (!ownerRecordValid) return false;

    const ble_gap_addr_t identity = ownerIdentityAddress(ownerRecord);
    if (addressesEqual(peer, identity)) return true;

    // Modern phones normally connect with a rotating Resolvable Private
    // Address (RPA).  The owner's IRK proves that such an address belongs to
    // the same bonded phone without the app sending a reusable owner token.
    return peer.addr_type == BLE_GAP_ADDR_TYPE_RANDOM_PRIVATE_RESOLVABLE &&
           Bluefruit.Security.resolveAddress(&peer, &ownerBond.peer_id.id_info);
}

static void stopPairingAdvertising() {
    if (pairingAdvHandle != BLE_GAP_ADV_SET_HANDLE_NOT_SET) {
        const uint32_t result = sd_ble_gap_adv_stop(pairingAdvHandle);
        if (result != NRF_SUCCESS && result != NRF_ERROR_INVALID_STATE) {
            rtt.printf("[BLE LOCK] advertising stop failed err=%lu\n",
                       (unsigned long)result);
        }
    }
    pairingAdvertisingRunning = false;
}

static bool clearControllerPeerFilters() {
    const uint32_t whitelistResult = sd_ble_gap_whitelist_set(nullptr, 0);
    const uint32_t identityResult =
        sd_ble_gap_device_identities_set(nullptr, nullptr, 0);
    if (whitelistResult != NRF_SUCCESS || identityResult != NRF_SUCCESS) {
        rtt.printf("[BLE LOCK] clear filters wl=%lu ids=%lu\n",
                   (unsigned long)whitelistResult,
                   (unsigned long)identityResult);
        return false;
    }
    return true;
}

static bool installOwnerPeerFilters() {
    if (!ownerRecordValid) return false;

    // The identity list lets the SoftDevice resolve the owner's rotating RPA;
    // the whitelist then rejects every other connection request in hardware.
    const ble_gap_id_key_t* identityKeys[] = { &ownerBond.peer_id };
    const uint32_t identityResult =
        sd_ble_gap_device_identities_set(identityKeys, nullptr, 1);
    if (identityResult != NRF_SUCCESS) {
        rtt.printf("[BLE LOCK] identity filter failed err=%lu\n",
                   (unsigned long)identityResult);
        return false;
    }

    const ble_gap_addr_t ownerAddress = ownerIdentityAddress(ownerRecord);
    const ble_gap_addr_t* whitelist[] = { &ownerAddress };
    const uint32_t whitelistResult = sd_ble_gap_whitelist_set(whitelist, 1);
    if (whitelistResult != NRF_SUCCESS) {
        rtt.printf("[BLE LOCK] whitelist failed err=%lu\n",
                   (unsigned long)whitelistResult);
        return false;
    }
    return true;
}

static void startAdvertising() {
    stopPairingAdvertising();

    if (connected || sleepShutdownRequested ||
        (pairingLockState != PairingLockState::PairingOpen &&
         pairingLockState != PairingLockState::OwnerLocked &&
         pairingLockState != PairingLockState::NoOwnerClosed)) {
        return;
    }

    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();

    const bool locked = pairingLockState == PairingLockState::OwnerLocked;
    const bool connectable = pairingLockState != PairingLockState::NoOwnerClosed;
    Bluefruit.Advertising.addFlags(
        locked ? BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED
               : BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(gService);

    // Manufacturer Specific Data wire format (little-endian company ID):
    //   bytes 0..1: Bluetooth SIG company identifier
    //   byte 2:     status format version
    //   byte 3:     flags (bit 0 owner bond valid, bit 1 pairing open)
    // Use ownerRecordValid rather than the advertising policy so the field
    // describes the verified persistent bond, not an inferred UI state.
    uint8_t statusFlags = 0u;
    if (ownerRecordValid) {
        statusFlags |= BLE_ADV_STATUS_OWNER_BONDED;
    }
    if (pairingLockState == PairingLockState::PairingOpen) {
        statusFlags |= BLE_ADV_STATUS_PAIRING_OPEN;
    }

    const uint8_t manufacturerData[] = {
        static_cast<uint8_t>(BLE_COMPANY_IDENTIFIER & 0xFFu),
        static_cast<uint8_t>((BLE_COMPANY_IDENTIFIER >> 8) & 0xFFu),
        BLE_ADV_STATUS_VERSION,
        statusFlags
    };
    if (!Bluefruit.Advertising.addManufacturerData(
            manufacturerData, sizeof(manufacturerData))) {
        rtt.println("[BLE LOCK] advertising kept off: status data did not fit");
        return;
    }
    Bluefruit.ScanResponse.addName();

    const bool filtersReady = locked ? installOwnerPeerFilters()
                                     : clearControllerPeerFilters();
    if (!filtersReady) {
        // Never fall back to open advertising when the owner filter fails.
        rtt.println("[BLE LOCK] advertising kept off: peer filter unavailable");
        return;
    }

    memset(&pairingAdvData, 0, sizeof(pairingAdvData));
    pairingAdvData.adv_data.p_data = Bluefruit.Advertising.getData();
    pairingAdvData.adv_data.len = Bluefruit.Advertising.count();
    pairingAdvData.scan_rsp_data.p_data = Bluefruit.ScanResponse.getData();
    pairingAdvData.scan_rsp_data.len = Bluefruit.ScanResponse.count();

    ble_gap_adv_params_t parameters = {};
    parameters.properties.type =
        connectable ? BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED
                    : BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED;
    parameters.interval = 244; // 152.5 ms, a good idle power/latency balance.
    parameters.duration = 0;   // State-machine timers own the open-window limit.
    parameters.filter_policy =
        locked ? BLE_GAP_ADV_FP_FILTER_CONNREQ : BLE_GAP_ADV_FP_ANY;
    parameters.primary_phy = BLE_GAP_PHY_AUTO;
    parameters.secondary_phy = BLE_GAP_PHY_AUTO;

    uint32_t result = sd_ble_gap_adv_set_configure(
        &pairingAdvHandle, &pairingAdvData, &parameters);
    if (result == NRF_SUCCESS) {
        result = sd_ble_gap_tx_power_set(
            BLE_GAP_TX_POWER_ROLE_ADV, pairingAdvHandle, Bluefruit.getTxPower());
    }
    if (result == NRF_SUCCESS) {
        result = sd_ble_gap_adv_start(pairingAdvHandle, CONN_CFG_PERIPHERAL);
    }

    pairingAdvertisingRunning = result == NRF_SUCCESS;
    rtt.printf("[BLE LOCK] advertising state=%s connectable=%u owner_only=%u status=0x%02X result=%lu\n",
               pairingAdvertisingRunning ? "on" : "off",
               connectable ? 1u : 0u, locked ? 1u : 0u, statusFlags,
               (unsigned long)result);
}

static void onBleConnect(uint16_t conn_handle) {
    pairingAdvertisingRunning = false;
    currentConnHandle = conn_handle;
    connected = true;
    applicationAuthorized = false;
    linkSecured = false;
    connectedPeerMatchesOwner = false;
    connectionHapticPending = false;
    connectionHapticPlayed = false;
    forceTelemetrySync = false;
    forceLiveSync = false;
    lastLiveSendMs = 0;
    lastTrainingTelemetrySendMs = 0;
    lastTherapyLiveSendMs = 0;
    lastStatusSendMs = 0;
    connectedSinceMs = 0UL;
    notificationsEnabled = false;
    applicationTxReadyLogged = false;
    therapyPlanSentForSession = false;
    lastTherapyPlanSessionId = 0;
    syncReset();

    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    if (!conn) {
        pendingUnauthorizedDisconnect = true;
    } else if (pairingLockState == PairingLockState::PairingOpen) {
        // The physical action opened enrollment.  Encryption is initiated
        // immediately; application data remains blocked until the resulting
        // bond has been persisted as the sole owner.
        claimAuthSucceeded = false;
        claimLinkSecured = false;
        pendingClaimIdentity = conn->getPeerAddr();
        securityDeadlineStartedMs = millis();
        const bool requested = conn->requestPairing();
        rtt.printf("[BLE LOCK] enrollment connected; pairing request=%u\n",
                   requested ? 1u : 0u);
    } else if (pairingLockState == PairingLockState::OwnerLocked) {
        connectedPeerMatchesOwner = peerMatchesStoredOwner(conn->getPeerAddr());
        if (connectedPeerMatchesOwner) {
            pairingLockState = PairingLockState::OwnerSecuring;
            securityDeadlineStartedMs = millis();
            const bool requested = conn->requestPairing();
            rtt.printf("[BLE LOCK] owner connected; encrypt request=%u\n",
                       requested ? 1u : 0u);
        } else {
            // This is a second line of defence behind the controller
            // whitelist, useful if a stack/configuration regression occurs.
            pendingUnauthorizedDisconnect = true;
            rtt.println("[BLE LOCK] rejected non-owner connection");
        }
    } else {
        pendingUnauthorizedDisconnect = true;
        rtt.println("[BLE LOCK] rejected connection in closed state");
    }

    turnRgbLedOff();
}

static void onBleDisconnect(uint16_t conn_handle, uint8_t reason) {
    (void)conn_handle;
    connected = false;
    currentConnHandle = BLE_CONN_HANDLE_INVALID;
    applicationAuthorized = false;
    linkSecured = false;
    connectedPeerMatchesOwner = false;
    notificationsEnabled = false;
    applicationTxReadyLogged = false;
    // Abandon any in-flight transfer: nothing was marked sent, so the whole
    // window simply re-streams on the next FETCH_SESSIONS.
    syncReset();
    connectionHapticPending = false;
    connectionHapticPlayed = false;
    forceLiveSync = false;
    connectedSinceMs = 0UL;
    if (remoteUnpairState == RemoteUnpairState::AckPending) {
        remoteUnpairState = RemoteUnpairState::Idle;
        remoteUnpairSeq = 0u;
        remoteUnpairQueuedMs = 0UL;
    }
    rtt.print("[BLE EVT] disconnected reason=0x");
    rtt.print(reason, HEX);
    rtt.print(" (");
    rtt.print(bleDisconnectReasonText(reason));
    rtt.println(")");

    if (pairingLockState == PairingLockState::OwnerAuthorized ||
        pairingLockState == PairingLockState::OwnerSecuring) {
        pairingLockState = PairingLockState::OwnerLocked;
    } else if (pairingLockState == PairingLockState::Revoking) {
        pendingUnlockFinalize = true;
    }

    if (sleepShutdownRequested) {
        sleepShutdownRequested = false;
        return;
    }

    // If the app closes the link after receiving the UNPAIR ACK, keep the
    // radio quiet until the 400 ms grace period finishes and revocation opens
    // unrestricted pairing. Do not briefly restart owner-only advertising.
    if (remoteUnpairState == RemoteUnpairState::AckSentWaiting) {
        return;
    }

    // Do not touch the SoftDevice advertiser from the callback task.  The
    // main loop performs the state-aware restart after this event unwinds.
    pendingAdvertisingRestart = true;
}

static void onBlePairComplete(uint16_t conn_handle, uint8_t auth_status) {
    (void)conn_handle;
    rtt.print("[BLE SEC] pairing complete status=0x");
    rtt.println(auth_status, HEX);

}

static void onBleSecured(uint16_t conn_handle) {
    pendingSecuredHandle = conn_handle;
    pendingSecuredEvent = true;
    rtt.println("[BLE SEC] link encrypted");
}

static void authorizeCurrentLink(unsigned long now) {
    if (!connected || currentConnHandle == BLE_CONN_HANDLE_INVALID ||
        !linkSecured || !ownerRecordValid) {
        return;
    }

    applicationAuthorized = true;
    pairingLockState = PairingLockState::OwnerAuthorized;
    connectedSinceMs = now;
    connectionHapticPending = BLE_CONNECTION_HAPTIC_ENABLED != 0;
    connectionHapticPlayed = false;
    forceTelemetrySync = true;
    forceLiveSync = true;
    securityDeadlineStartedMs = 0UL;
    turnRgbLedOff();

    // Link tuning starts only after identity and encryption are established;
    // it cannot accidentally grant an unauthenticated connection more time.
    BLEConnection* conn = Bluefruit.Connection(currentConnHandle);
    if (conn) {
        bool dlRequested = false;
        bool phyRequested = false;
#if BLE_REQUEST_DATA_LENGTH_ON_CONNECT
        dlRequested = conn->requestDataLengthUpdate();
#endif
#if BLE_REQUEST_PHY_ON_CONNECT
        phyRequested = conn->requestPHY();
#endif
        rtt.printf("[BLE LOCK] owner authorized h=%u dl=%u phy=%u\n",
                   currentConnHandle, dlRequested ? 1u : 0u,
                   phyRequested ? 1u : 0u);
    }
}

static void initializePairingLock() {
    BleOwnerRecord stored = {};
    const bool recordLoaded = loadOwnerRecord(stored);

    if (recordLoaded &&
        (stored.flags & BLE_OWNER_REVOCATION_PENDING) == 0u &&
        loadAndVerifyOwnerBond(stored, ownerBond)) {
        ownerRecord = stored;
        ownerRecordValid = true;
        pairingLockState = PairingLockState::OwnerLocked;
        rtt.println("[BLE LOCK] persisted owner verified; lock enabled");
        return;
    }

    // A missing/corrupt record, a missing bond, or an interrupted revocation
    // is deliberately fail-closed.  Legacy one-byte pairing markers are not
    // owner identities, so their bonds are removed during this migration.
    Bluefruit.Periph.clearBonds();
    clearOwnerRecordFiles();
    memset(&ownerRecord, 0, sizeof(ownerRecord));
    memset(&ownerBond, 0, sizeof(ownerBond));
    ownerRecordValid = false;
    pairingLockState = PairingLockState::NoOwnerClosed;
    rtt.println("[BLE LOCK] no verified owner; double-click in IDLE to pair");
}

static void finalizePairingUnlock(unsigned long now) {
    if (connected) return;

    const unsigned long previousWindowStart = pairingWindowStartedMs;
    const bool preserveWindow = preservePairingWindowOnCleanup;
    const bool reusePreviousWindow = preserveWindow && previousWindowStart != 0UL;
    const bool closeAfterCleanup = closePairingWindowAfterCleanup;

    stopPairingAdvertising();
    clearControllerPeerFilters();
    Bluefruit.Periph.clearBonds();
    clearOwnerRecordFiles();
    memset(&ownerRecord, 0, sizeof(ownerRecord));
    memset(&ownerBond, 0, sizeof(ownerBond));
    ownerRecordValid = false;
    applicationAuthorized = false;
    connectedPeerMatchesOwner = false;
    linkSecured = false;
    claimAuthSucceeded = false;
    claimLinkSecured = false;
    claimStartedMs = 0UL;
    securityDeadlineStartedMs = 0UL;
    pendingUnlockFinalize = false;
    pendingUnauthorizedDisconnect = false;
    pendingSecuredEvent = false;
    pendingAuthEvent = false;
    preservePairingWindowOnCleanup = false;
    closePairingWindowAfterCleanup = false;

    if (closeAfterCleanup) {
        pairingWindowStartedMs = 0UL;
        pairingLockState = PairingLockState::NoOwnerClosed;
        turnRgbLedOff();
        rtt.println("[BLE LOCK] pairing window closed; background advertising resumed");
        startAdvertising();
        return;
    }

    // A failed enrollment attempt must not extend the physical-access window.
    // A fresh button unlock gets a new full window; cleanup after a failed
    // claim reuses the original deadline.
    if (reusePreviousWindow &&
        (uint32_t)(now - previousWindowStart) >= BLE_PAIRING_WINDOW_MS) {
        pairingWindowStartedMs = 0UL;
        pairingLockState = PairingLockState::NoOwnerClosed;
        turnRgbLedOff();
        rtt.println("[BLE LOCK] failed claim cleanup reached window deadline");
        startAdvertising();
        return;
    }

    pairingWindowStartedMs = reusePreviousWindow ? previousWindowStart : now;
    pairingLockState = PairingLockState::PairingOpen;

    const unsigned long elapsed =
        reusePreviousWindow ? (uint32_t)(now - pairingWindowStartedMs) : 0UL;
    rtt.printf("[BLE LOCK] pairing window open for %lu ms\n",
               (unsigned long)(BLE_PAIRING_WINDOW_MS - elapsed));
    startAdvertising();
}

static void abandonOwnerClaim(unsigned long now, const char* reason) {
    rtt.printf("[BLE LOCK] owner claim abandoned: %s\n", reason);
    pairingLockState = PairingLockState::Revoking;
    pendingUnlockFinalize = true;
    claimAuthSucceeded = false;
    claimLinkSecured = false;
    claimStartedMs = now;
    preservePairingWindowOnCleanup = true;
    closePairingWindowAfterCleanup = false;

    if (connected && currentConnHandle != BLE_CONN_HANDLE_INVALID) {
        Bluefruit.disconnect(currentConnHandle);
    } else {
        finalizePairingUnlock(now);
    }
}

static void processPairingLock(unsigned long now) {
    if (pendingPairingWindowClose) {
        pendingPairingWindowClose = false;
        if (pairingLockState == PairingLockState::PairingOpen ||
            pairingLockState == PairingLockState::ClaimCommitting) {
            stopPairingAdvertising();
            pairingWindowStartedMs = 0UL;
            pairingLockState = PairingLockState::Revoking;
            pendingUnlockFinalize = true;
            preservePairingWindowOnCleanup = false;
            closePairingWindowAfterCleanup = true;
            applicationAuthorized = false;
            turnRgbLedOff();

            if (connected && currentConnHandle != BLE_CONN_HANDLE_INVALID) {
                Bluefruit.disconnect(currentConnHandle);
                return;
            }
            finalizePairingUnlock(now);
        }
    }

    if (pendingUnauthorizedDisconnect) {
        pendingUnauthorizedDisconnect = false;
        applicationAuthorized = false;
        linkSecured = false;
        pendingSecuredEvent = false;
        pendingAuthEvent = false;
        if (connected && currentConnHandle != BLE_CONN_HANDLE_INVALID) {
            rtt.println("[BLE LOCK] disconnecting unauthorized peer");
            Bluefruit.disconnect(currentConnHandle);
            // A security-update event may already be queued behind a rejected
            // pairing request.  Never evaluate it in the same loop iteration.
            return;
        }
    }

    if (pendingUnlockFinalize && !connected) {
        finalizePairingUnlock(now);
    }

    if (pendingSecuredEvent) {
        const uint16_t securedHandle = pendingSecuredHandle;
        pendingSecuredEvent = false;
        pendingSecuredHandle = BLE_CONN_HANDLE_INVALID;

        if (connected && securedHandle == currentConnHandle) {
            linkSecured = true;
            if (pairingLockState == PairingLockState::OwnerSecuring &&
                connectedPeerMatchesOwner) {
                authorizeCurrentLink(now);
            } else if (pairingLockState == PairingLockState::PairingOpen ||
                       pairingLockState == PairingLockState::ClaimCommitting) {
                claimLinkSecured = true;
            }
        }
    }

    if (pendingAuthEvent) {
        const uint8_t authStatus = pendingAuthStatus;
        const bool bonded = pendingAuthBonded;
        pendingAuthEvent = false;

        if (pairingLockState == PairingLockState::PairingOpen ||
            pairingLockState == PairingLockState::ClaimCommitting) {
            if (authStatus == BLE_GAP_SEC_STATUS_SUCCESS && bonded) {
                claimAuthSucceeded = true;
                pairingLockState = PairingLockState::ClaimCommitting;
                claimStartedMs = now;
                lastClaimLoadAttemptMs = 0UL;
                rtt.println("[BLE LOCK] encrypted bond complete; committing owner");
            } else {
                abandonOwnerClaim(now, "pairing failed or peer refused bonding");
            }
        }
    }

    if (pairingLockState == PairingLockState::ClaimCommitting &&
        claimAuthSucceeded && claimLinkSecured &&
        (lastClaimLoadAttemptMs == 0UL ||
         (uint32_t)(now - lastClaimLoadAttemptMs) >= 50UL)) {
        lastClaimLoadAttemptMs = now;
        bond_keys_t newBond = {};

        // Bluefruit saves bonds on its callback queue.  Poll briefly until
        // that atomic bond file is visible, then bind our owner record to it.
        if (bond_load_keys(BLE_GAP_ROLE_PERIPH,
                           &pendingClaimIdentity, &newBond)) {
            BleOwnerRecord newRecord = {};
            fillOwnerRecord(newRecord, newBond.peer_id.id_addr_info,
                            newBond, BLE_OWNER_VALID);
            if (saveOwnerRecord(newRecord)) {
                ownerRecord = newRecord;
                ownerBond = newBond;
                ownerRecordValid = true;
                claimAuthSucceeded = false;
                claimLinkSecured = false;
                rtt.println("[BLE LOCK] owner committed; future connections owner-only");

                if (connected) {
                    connectedPeerMatchesOwner = true;
                    authorizeCurrentLink(now);
                } else {
                    pairingLockState = PairingLockState::OwnerLocked;
                    startAdvertising();
                }
            } else {
                abandonOwnerClaim(now, "owner record write failed");
            }
        }
    }

    if (pairingLockState == PairingLockState::ClaimCommitting &&
        claimStartedMs != 0UL &&
        (uint32_t)(now - claimStartedMs) >= BLE_OWNER_CLAIM_TIMEOUT_MS) {
        abandonOwnerClaim(now, "bond-store commit timed out");
    }

    if (pairingLockState == PairingLockState::PairingOpen && connected &&
        securityDeadlineStartedMs != 0UL &&
        (uint32_t)(now - securityDeadlineStartedMs) >= BLE_SECURITY_TIMEOUT_MS) {
        abandonOwnerClaim(now, "encryption timed out");
    }

    if (pairingLockState == PairingLockState::OwnerSecuring && connected &&
        securityDeadlineStartedMs != 0UL &&
        (uint32_t)(now - securityDeadlineStartedMs) >= BLE_SECURITY_TIMEOUT_MS) {
        pendingUnauthorizedDisconnect = true;
        rtt.println("[BLE LOCK] owner encryption timed out");
    }

    if (pairingLockState == PairingLockState::PairingOpen &&
        pairingWindowStartedMs != 0UL &&
        (uint32_t)(now - pairingWindowStartedMs) >= BLE_PAIRING_WINDOW_MS) {
        pairingLockState = PairingLockState::NoOwnerClosed;
        stopPairingAdvertising();
        if (connected && currentConnHandle != BLE_CONN_HANDLE_INVALID) {
            Bluefruit.disconnect(currentConnHandle);
        }
        turnRgbLedOff();
        rtt.println("[BLE LOCK] pairing window expired; background advertising resumed");
        if (!connected) {
            startAdvertising();
        }
    }

    if (pendingAdvertisingRestart && !connected) {
        pendingAdvertisingRestart = false;
        startAdvertising();
    }
}

static bool processRemoteUnpair(unsigned long now) {
    const RemoteUnpairState state = remoteUnpairState;
    if (state == RemoteUnpairState::Idle) {
        return false;
    }

    if (state == RemoteUnpairState::AckPending) {
        // Revalidate in the main task before accepting the destructive action.
        // RX is already owner-gated, but this also handles a link/state change
        // between the BLE callback and this loop iteration.
        const bool verifiedOwner =
            connected && applicationAuthorized && ownerRecordValid &&
            connectedPeerMatchesOwner &&
            pairingLockState == PairingLockState::OwnerAuthorized;
        if (!verifiedOwner) {
            remoteUnpairState = RemoteUnpairState::Idle;
            remoteUnpairSeq = 0u;
            remoteUnpairQueuedMs = 0UL;
            rtt.println("[BLE LOCK] remote unlock canceled before ACK");
            return false;
        }

        if ((uint32_t)(now - remoteUnpairQueuedMs) >=
            REMOTE_UNPAIR_ACK_TIMEOUT_MS) {
            remoteUnpairState = RemoteUnpairState::Idle;
            remoteUnpairSeq = 0u;
            remoteUnpairQueuedMs = 0UL;
            rtt.println("[BLE LOCK] remote unlock canceled: ACK timeout");
            return false;
        }

        const uint32_t seq = remoteUnpairSeq;
        if (sendCommandAck(seq, "UNPAIR", true)) {
            remoteUnpairAckSentMs = now;
            remoteUnpairQueuedMs = 0UL;
            remoteUnpairState = RemoteUnpairState::AckSentWaiting;
            rtt.printf("[BLE LOCK] remote unlock ACK sent seq=%lu\n",
                       (unsigned long)seq);
        }
        return true;
    }

    if ((uint32_t)(now - remoteUnpairAckSentMs) <
        REMOTE_UNPAIR_DISCONNECT_DELAY_MS) {
        return true;
    }

    remoteUnpairState = RemoteUnpairState::Idle;
    remoteUnpairSeq = 0u;
    remoteUnpairQueuedMs = 0UL;
    remoteUnpairAckSentMs = 0UL;
    beginPairingUnlock("remote");
    return true;
}

static bool applyMode(const String &valueRaw) {
    if (isCalibrating()) {
        // rtt.println("BLE CMD: MODE change ignored - calibration in progress");
        return false;
    }

    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    if (value == "TRAINING" || value == "POSTURE") {
        deviceOn = true;
        setDeviceMode(MODE_TRAINING);
        forceTelemetrySync = true;
        forceLiveSync = true;
        // RTT trace disabled here on purpose.
        // Uncomment if you want bluetooth.cpp to emit BLE state changes to RTT.
        // logEvent("BLE", "mode_training");
        return true;
    } else if (value == "THERAPY") {
        deviceOn = true;
        setDeviceMode(MODE_THERAPY);
        forceTelemetrySync = true;
        forceLiveSync = true;
        return true;
    } else if (value == "IDLE" || value == "OFF") {
        setDeviceMode(MODE_IDLE);
        forceTelemetrySync = true;
        forceLiveSync = true;
        return true;
    }
    return false;
}

static bool parseTrainingSubMode(const String& valueRaw, TrainingAlertStyle& out) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    if (value == "INSTANT") {
        out = TrainingAlertStyle::Instant;
        return true;
    }
    if (value == "DELAYED") {
        out = TrainingAlertStyle::Delayed;
        return true;
    }
    if (value == "NO_ALERTS" || value == "NO_ALERT" ||
        value == "NO ALERTS" || value == "NO ALERT") {
        out = TrainingAlertStyle::NoAlerts;
        return true;
    }
    return false;
}

static bool extractJsonStringField(const String& payload, const char* key, String& out);
static bool extractJsonIntField(const String& payload, const char* key, uint32_t& out);

static bool extractTrainingStartFields(const String& payload,
                                       TrainingAlertStyle& subMode,
                                       uint32_t& difficultyAngle,
                                       uint32_t& delayMs,
                                       const char*& error) {
    String subModeRaw;
    if (!extractJsonStringField(payload, "sub_mode", subModeRaw) &&
        !extractJsonStringField(payload, "sub-mode", subModeRaw)) {
        error = "BAD_REQUEST";
        return false;
    }
    if (!parseTrainingSubMode(subModeRaw, subMode)) {
        error = "BAD_SUB_MODE";
        return false;
    }

    if (!extractJsonIntField(payload, "difficulty_angle", difficultyAngle) &&
        !extractJsonIntField(payload, "difficulty-angle", difficultyAngle)) {
        error = "BAD_REQUEST";
        return false;
    }
    if (difficultyAngle < 5u || difficultyAngle > 60u) {
        error = "BAD_DIFFICULTY";
        return false;
    }

    if (!extractJsonIntField(payload, "delay_ms", delayMs) &&
        !extractJsonIntField(payload, "delay-ms", delayMs)) {
        error = "BAD_REQUEST";
        return false;
    }
    if (delayMs < 500u || delayMs > 30000u) {
        error = "BAD_DELAY";
        return false;
    }

    return true;
}

static bool startTrainingFromBle(TrainingAlertStyle subMode,
                                 uint32_t difficultyAngle,
                                 uint32_t delayMs,
                                 const char*& error) {
    if (isCalibrating()) {
        error = "CALIBRATING";
        return false;
    }

    trainingSubModeIndex = subMode;
    kBadPostureDeg = (float)difficultyAngle;
    trainingDelayedAlertMs = delayMs;
    deviceOn = true;
    setDeviceMode(MODE_TRAINING);
    markSubModeChanged();
    bluetoothNotifyStateChanged();
    return true;
}

static bool startTherapyFromBle(uint32_t intensity, uint32_t durationMin, const char*& error) {
    if (isCalibrating()) {
        error = "CALIBRATING";
        return false;
    }
    if (therapyIsRunning()) {
        error = "ALREADY_RUNNING";
        return false;
    }

    // Keep accepting the legacy duration choices so existing app versions can
    // start therapy; the therapy engine always applies the fixed 20 minutes.
    if (durationMin != 10 && durationMin != 20 && durationMin != 30) {
        error = "BAD_DURATION";
        return false;
    }

    if (intensity < 1 || intensity > 3) {
        error = "BAD_INTENSITY";
        return false;
    }

    therapyIntensityLevel = (int)intensity;
    therapySubModeIndex = 1;

    deviceOn = true;
    setDeviceMode(MODE_THERAPY);
    markSubModeChanged();
    bluetoothNotifyStateChanged();
    return true;
}

static void stopTherapyFromBle() {
    setDeviceMode(MODE_IDLE);
    forceTelemetrySync = true;
    forceLiveSync = true;
}

static bool trainingtop() {
    if (isCalibrating()) {
        return false;
    }

    setDeviceMode(MODE_IDLE);
    deviceOn = true;
    forceTelemetrySync = true;
    forceLiveSync = true;
    bluetoothNotifyStateChanged();
    return true;
}

static void applyCalibrationControl(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    value.toUpperCase();

    if (value == "START") {
        if (isCalibrating()) return;
        calibrationRequestStart();
        // logEvent("BLE", "calibration_start");
    } else if (value == "CANCEL") {
        if (!isCalibrating()) return;
        calibrationRequestCancel();
        // logEvent("BLE", "calibration_cancel");
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
        // logEvent("BLE", "action_calibrate");
    } else if (value == "CALIBRATE_CANCEL") {
        if (!isCalibrating()) return;
        calibrationRequestCancel();
        // logEvent("BLE", "action_calibrate_cancel");
    } else if (value == "STOP_THERAPY" || value == "THERAPY_STOP" ||
               value == "STOP THERAPY" || value == "STOP") {
        stopTherapyFromBle();
    } else if (value == "ENTER_DFU" || value == "OTA_DFU" || value == "DFU") {
        if (pendingDfuEnter) return;
        pendingDfuEnter = true;
        notifyDfuStatus("ARMED");
        // logEvent("BLE", "action_enter_dfu");
    } else if (value == "GET_DEVICE_INFO") {
        sendDeviceInfoPacket();
        // logEvent("BLE", "action_device_info");
    } else if (value == "PROFILE_CLEAR" || value == "CLEAR_PROFILES") {
        clearCalibrationProfiles();
        // logEvent("BLE", "action_profile_clear");
    } else if (value == "FACTORY_RESET") {
        pendingFactoryReset = true;
        // logEvent("BLE", "action_factory_reset");
    }
}

static void sendProfileList() {
    if (!pCharacteristic || !connected) return;
    char payload[1024];
    char profilesJson[800];
    profilesJson[0] = '\0';
    strncat(profilesJson, "[", sizeof(profilesJson) - 1);
    for (uint8_t i = 0; i < getProfileCount(); i++) {
        const OrientationProfile* p = getProfile(i);
        if (!p || !p->valid) continue;
        int quality = (int)(p->stabilityScore);
        if (quality < 0) quality = 0;
        if (quality > 100) quality = 100;

        char item[96];
        snprintf(item, sizeof(item),
                 "%s{\"id\":%lu,\"n\":\"%s\",\"c\":%lu,\"q\":%d}",
                 (profilesJson[1] == '\0') ? "" : ",",
                 (unsigned long)p->id,
                 p->name,
                 (unsigned long)p->createdAtEpoch,
                 quality);
        strncat(profilesJson, item, sizeof(profilesJson) - strlen(profilesJson) - 1);
    }
    strncat(profilesJson, "]", sizeof(profilesJson) - strlen(profilesJson) - 1);
    // Active/default are reported once by id ("active"/"default", 0 = system
    // default) instead of per-profile flags.
    const OrientationProfile* activeProfile = getActiveProfile();
    snprintf(payload, sizeof(payload), "{\"t\":\"P\",\"profiles\":%s,\"count\":%u,\"max\":8,\"active\":%lu,\"default\":%lu}",
             profilesJson,
             (unsigned)getProfileCount(),
             (unsigned long)(activeProfile ? activeProfile->id : 0u),
             (unsigned long)getDefaultProfileId());
    sendBlePacket(payload);
}

static void sendCalibrationDone(bool success, uint32_t profileId = 0, const char* name = nullptr, uint16_t quality = 0, uint16_t samples = 0, const char* reason = nullptr, float refX = 0.0f, float refY = 0.0f, float refZ = 0.0f, uint16_t passedSamples = 0) {
    if (!pCharacteristic || !connected) return;
    char payload[320];
    if (success) {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"C\",\"state\":\"DONE\",\"result\":\"success\",\"save_state\":\"pending\",\"profile_id\":%lu,\"name\":\"%s\",\"quality\":%u,\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"total_samples\":%u,\"passed_samples\":%u}",
                 (unsigned long)profileId,
                 name ? name : "",
                 (unsigned)quality,
                 refX,
                 refY,
                 refZ,
                 (unsigned)samples,
                 (unsigned)passedSamples);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"C\",\"state\":\"DONE\",\"result\":\"failed\",\"reason\":\"%s\"}",
                 reason ? reason : "MOVEMENT_TOO_HIGH");
    }
    sendBlePacket(payload);
}

void notifyCalibrationComplete(bool success, uint32_t profileId, const char* name, uint16_t quality, uint16_t sampleCount, const char* reason, float refX, float refY, float refZ, uint16_t passedSamples) {
    sendCalibrationDone(success, profileId, name, quality, sampleCount, reason, refX, refY, refZ, passedSamples);
}

void notifyCalibrationPersisted(uint32_t profileId, bool success, uint32_t totalMs, uint32_t flashMs) {
    if (!pCharacteristic || !connected) return;
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"C\",\"state\":\"SAVED\",\"profile_id\":%lu,\"persisted\":%s,\"save_ms\":%lu,\"flash_ms\":%lu}",
             (unsigned long)profileId,
             success ? "true" : "false",
             (unsigned long)totalMs,
             (unsigned long)flashMs);
    sendBlePacket(payload);
}

static void applyTimeSync(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    long epoch = value.toInt();
    if (epoch > 0) {
        setDeviceTime(epoch);
        char payload[48];
        snprintf(payload, sizeof(payload), "{\"time\":%ld}", epoch);
        // logPacket("BLE", payload);
    }
}

static void applyTZOffset(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    long tz = value.toInt();
    setDeviceTZOffset(tz);
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"tz\":%ld}", tz);
    // logPacket("BLE", payload);
}



static void applyDifficultyDegrees(const String &valueRaw) {
    String value = valueRaw;
    value.trim();
    int deg = value.toInt();
    if (deg >= 5 && deg <= 60) {
        kBadPostureDeg = (float)deg;
        char payload[48];
        snprintf(payload, sizeof(payload), "{\"difficulty_deg\":%d}", deg);
        // logPacket("BLE", payload);
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
        // logEvent("BLE", "profile_clear");
        return;
    }

    if (upper == "DEFAULT") {
        selectDefaultCalibrationProfile();
        // logEvent("BLE", "profile_default");
        return;
    }

    int requestedIndex = value.toInt();
    if (requestedIndex > 0) {
        if (selectCalibrationProfile((uint8_t)(requestedIndex - 1))) {
            char payload[64];
            snprintf(payload, sizeof(payload), "{\"profile_index\":%d}", requestedIndex);
            // logPacket("BLE", payload);
        } else {
            char payload[80];
            snprintf(payload, sizeof(payload), "{\"profile_index\":%d,\"ignored\":true}", requestedIndex);
            // logPacket("BLE", payload);
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
            // logPacket("BLE", payload);
            return;
        }
    }

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"profile\":\"%s\",\"ignored\":true}", value.c_str());
    // logPacket("BLE", payload);
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

static uint8_t extractJsonIntArray(const String& payload, const char* key,
                                   uint16_t* out, uint8_t maxOut) {
    String pattern = String("\"") + key + "\"";
    int keyPos = payload.indexOf(pattern);
    if (keyPos < 0) return 0;
    int open = payload.indexOf('[', keyPos);
    if (open < 0) return 0;
    int close = payload.indexOf(']', open);
    if (close < 0) return 0;

    uint8_t n = 0;
    int i = open + 1;
    while (i < close && n < maxOut) {
        while (i < close && !isdigit((unsigned char)payload[i])) i++;
        if (i >= close) break;
        int numStart = i;
        while (i < close && isdigit((unsigned char)payload[i])) i++;
        out[n++] = (uint16_t)payload.substring(numStart, i).toInt();
    }
    return n;
}

// ── Session sync FSM implementation ──────────────────────────────────────

#if ALIGN_RTT_SYNC_LOG
#define SYNC_LOG(...) rtt.printf(__VA_ARGS__)
#else
#define SYNC_LOG(...)
#endif

static unsigned long syncLastProgressMs = 0;
static constexpr uint32_t SYNC_STALL_TIMEOUT_MS = 30000UL;

static void syncReset() {
    syncState = SyncState::Idle;
    syncCount = 0;
    syncEntryIdx = 0;
    syncChunkIdx = 0;
    syncSeq = 0;
    syncRetxCount = 0;
    syncRetxIdx = 0;
    syncAckReceived = false;
    syncAckOk = false;
    syncFetchRequested = false;
    syncEvLoadedFor = -1;
    syncFastParamsActive = false;
}

static void syncRequestConnInterval(uint16_t intervalUnits) {
    if (currentConnHandle == BLE_CONN_HANDLE_INVALID) return;
    BLEConnection* conn = Bluefruit.Connection(currentConnHandle);
    if (conn) conn->requestConnectionParameter(intervalUnits);
}

// Ends a transfer (committed, aborted, or timed out) and relaxes the
// connection interval if we sped it up for streaming.
static void syncFinish(const char* why) {
    (void)why;
    SYNC_LOG("[SYNC] finish: %s\n", why);
    if (syncFastParamsActive) {
        syncRequestConnInterval(SYNC_IDLE_CONN_INTERVAL);
    }
    syncReset();
}

// Loads event detail for one window entry into the shared cache and fills
// in evCount/evChunks. Chunks are sized so every SEV packet fits a single
// notification for the MTU captured at transfer start.
static void syncLoadEvents(uint8_t idx) {
    if (syncEvLoadedFor == (int)idx) return;
    SyncEntry& e = syncWindow[idx];
    e.evCount = 0;
    e.evChunks = 0;

    if (e.s.start_ts != 0) {
        if (e.s.type == SESSION_TYPE_POSTURE &&
            session_log_read_posture_events(e.s.start_ts, syncPostureEv)) {
            e.evCount = syncPostureEv.pairCount;
            e.evChunks = (e.evCount + syncPairsPerChunk - 1) / syncPairsPerChunk;
        } else if (e.s.type == SESSION_TYPE_THERAPY &&
                   session_log_read_therapy_events(e.s.start_ts, syncTherapyEv)) {
            e.evCount = syncTherapyEv.count;
            e.evChunks = (e.evCount + syncTherapyPerChunk - 1) / syncTherapyPerChunk;
        }
    }
    syncEvLoadedFor = (int)idx;
}

static bool syncSendSummary(uint8_t idx, uint16_t seq) {
    syncLoadEvents(idx);
    const SyncEntry& e = syncWindow[idx];
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"t\":\"SESS\",\"x\":%lu,\"q\":%u,\"ty\":%u,\"ts\":%lu,\"sy\":%u,"
             "\"d\":%u,\"wc\":%u,\"wd\":%u,\"pat\":%u,\"ev\":%u,\"ec\":%u}",
             (unsigned long)syncXferId, (unsigned)seq, (unsigned)e.s.type,
             (unsigned long)e.s.start_ts, e.s.ts_synced ? 1u : 0u,
             (unsigned)e.s.duration_sec, (unsigned)e.s.wrong_count,
             (unsigned)e.s.wrong_dur_sec, (unsigned)e.s.therapy_pattern,
             (unsigned)e.evCount, (unsigned)e.evChunks);
    return sendBlePacket(payload);
}

static bool syncSendEventChunk(uint8_t idx, uint8_t chunk, uint16_t seq) {
    syncLoadEvents(idx);
    const SyncEntry& e = syncWindow[idx];
    if (chunk >= e.evChunks) return true;  // nothing to send; treat as done

    char payload[336];
    int pos = snprintf(payload, sizeof(payload),
                       "{\"t\":\"SEV\",\"x\":%lu,\"q\":%u,\"ts\":%lu,\"ty\":%u,"
                       "\"i\":%u,\"n\":%u,\"e\":[",
                       (unsigned long)syncXferId, (unsigned)seq,
                       (unsigned long)e.s.start_ts, (unsigned)e.s.type,
                       (unsigned)chunk, (unsigned)e.evChunks);

    if (e.s.type == SESSION_TYPE_POSTURE) {
        uint16_t first = (uint16_t)chunk * syncPairsPerChunk;
        uint16_t last = first + syncPairsPerChunk;
        if (last > e.evCount) last = e.evCount;
        for (uint16_t i = first; i < last; i++) {
            pos += snprintf(payload + pos, sizeof(payload) - pos, "%s[%u,%u]",
                            (i == first) ? "" : ",",
                            (unsigned)syncPostureEv.slouchOffsets[i],
                            (unsigned)syncPostureEv.correctionOffsets[i]);
        }
    } else {
        uint16_t first = (uint16_t)chunk * syncTherapyPerChunk;
        uint16_t last = first + syncTherapyPerChunk;
        if (last > e.evCount) last = e.evCount;
        for (uint16_t i = first; i < last; i++) {
            pos += snprintf(payload + pos, sizeof(payload) - pos, "%s%u",
                            (i == first) ? "" : ",",
                            (unsigned)syncTherapyEv.patterns[i]);
        }
    }
    snprintf(payload + pos, sizeof(payload) - pos, "]}");
    return sendBlePacket(payload);
}

// Retransmits one nacked seq by regenerating the packet from the window.
// Unknown seqs are skipped (returns true) so a bad NACK can't wedge the FSM.
static bool syncResendSeq(uint16_t seq) {
    for (uint8_t i = 0; i < syncCount; i++) {
        const SyncEntry& e = syncWindow[i];
        if (seq < e.seqBase || seq > (uint16_t)(e.seqBase + e.evChunks)) continue;
        if (seq == e.seqBase) return syncSendSummary(i, seq);
        return syncSendEventChunk(i, (uint8_t)(seq - e.seqBase - 1), seq);
    }
    return true;
}

// Freezes the unsent window and starts a new transfer. Called from the main
// loop only (file I/O + window copy are not BLE-callback safe).
static void syncBeginTransfer() {
    syncFetchRequested = false;
    if (syncFastParamsActive) {
        syncRequestConnInterval(SYNC_IDLE_CONN_INTERVAL);
    }
    syncReset();
    if (!connected || !pCharacteristic) return;

    uint16_t mtu = 23;
    BLEConnection* conn = Bluefruit.Connection(currentConnHandle);
    if (conn) mtu = conn->getMtu();

    if (mtu < SYNC_MIN_MTU) {
        char payload[96];
        snprintf(payload, sizeof(payload),
                 "{\"t\":\"SESS_HDR\",\"x\":0,\"n\":0,\"err\":\"MTU_TOO_SMALL\",\"mtu\":%u}",
                 (unsigned)mtu);
        sendBlePacket(payload);
        return;
    }

    // Chunk capacities fixed for the whole transfer: every packet must fit
    // one notification so a failed notify() is retried cleanly (no tearing).
    const uint16_t budget = (mtu - 3) - 84;      // usable minus JSON envelope
    syncPairsPerChunk = budget / 14;             // "[65535,65535]," per pair
    if (syncPairsPerChunk < 4) syncPairsPerChunk = 4;
    if (syncPairsPerChunk > 16) syncPairsPerChunk = 16;
    syncTherapyPerChunk = budget / 4;            // "255," per pattern
    if (syncTherapyPerChunk < 8) syncTherapyPerChunk = 8;
    if (syncTherapyPerChunk > MAX_THERAPY_PATTERNS) syncTherapyPerChunk = MAX_THERAPY_PATTERNS;

    StoredSession s;
    int fileIndex = 0;
    while (syncCount < SYNC_MAX_WINDOW &&
           session_log_get_unsent(syncCount, s, fileIndex)) {
        syncWindow[syncCount].s = s;
        syncWindow[syncCount].seqBase = 0;
        syncWindow[syncCount].evCount = 0;
        syncWindow[syncCount].evChunks = 0;
        syncCount++;
    }

    syncXferId = millis() | 1u;  // nonzero transfer id
    syncState = SyncState::Hdr;
    syncLastProgressMs = millis();
    SYNC_LOG("[SYNC] begin xfer=%lu window=%u mtu=%u pairs/chunk=%u\n",
             (unsigned long)syncXferId, (unsigned)syncCount, (unsigned)mtu,
             (unsigned)syncPairsPerChunk);
}

// Commits a fully-acked transfer: batch-marks the window sent (tiny marker
// appends, flushed together by session_log_loop) and compacts the log only
// once everything unsent has drained.
static void syncCommitWindow() {
    for (uint8_t i = 0; i < syncCount; i++) {
        session_log_mark_sent_matching(syncWindow[i].s);
    }
    if (session_log_count_unsent() == 0) {
        session_log_purge_sent();
    }
    SYNC_LOG("[SYNC] committed %u sessions, %d unsent remain\n",
             (unsigned)syncCount, session_log_count_unsent());
}

// Advances the sync FSM by at most one packet per call. `txBusy` means a
// higher-priority packet (telemetry/live) already used this tick.
static void syncProcess(unsigned long now, bool txBusy) {
    switch (syncState) {
        case SyncState::Idle:
            if (sessAvailPending && connected && !txBusy) {
                char payload[64];
                snprintf(payload, sizeof(payload),
                         "{\"t\":\"SESS_AVAIL\",\"n\":%d}",
                         session_log_count_unsent());
                if (sendBlePacket(payload)) sessAvailPending = false;
            }
            return;

        case SyncState::Hdr: {
            if (txBusy) break;
            char payload[96];
            snprintf(payload, sizeof(payload),
                     "{\"t\":\"SESS_HDR\",\"x\":%lu,\"n\":%u,\"total\":%d}",
                     (unsigned long)syncXferId, (unsigned)syncCount,
                     session_log_count_unsent());
            if (!sendBlePacket(payload)) break;
            syncLastProgressMs = now;
            if (syncCount == 0) {
                syncFinish("empty");
                return;
            }
            syncRequestConnInterval(SYNC_FAST_CONN_INTERVAL);
            syncFastParamsActive = true;
            syncState = SyncState::Stream;
            break;
        }

        case SyncState::Stream: {
            if (txBusy) break;
            SyncEntry& e = syncWindow[syncEntryIdx];
            bool sent;
            if (syncChunkIdx == 0) {
                e.seqBase = syncSeq;
                sent = syncSendSummary(syncEntryIdx, syncSeq);
            } else {
                sent = syncSendEventChunk(syncEntryIdx, syncChunkIdx - 1, syncSeq);
            }
            if (!sent) break;  // FIFO full or CCCD off: retry same seq next tick
            syncLastProgressMs = now;
            syncSeq++;
            syncChunkIdx++;
            if (syncChunkIdx > e.evChunks) {
                syncChunkIdx = 0;
                syncEntryIdx++;
            }
            if (syncEntryIdx >= syncCount) {
                syncState = SyncState::End;
            }
            break;
        }

        case SyncState::End: {
            if (txBusy) break;
            char payload[96];
            snprintf(payload, sizeof(payload),
                     "{\"t\":\"SESS_END\",\"x\":%lu,\"pk\":%u,\"n\":%u}",
                     (unsigned long)syncXferId, (unsigned)syncSeq,
                     (unsigned)syncCount);
            if (!sendBlePacket(payload)) break;
            syncLastProgressMs = now;
            syncAckWaitStartMs = now;
            syncState = SyncState::WaitAck;
            break;
        }

        case SyncState::WaitAck:
            if (syncAckReceived) {
                syncAckReceived = false;
                syncLastProgressMs = now;
                if (syncRetxCount > 0) {
                    syncRetxIdx = 0;
                    syncState = SyncState::Retx;
                } else if (syncAckOk) {
                    syncCommitWindow();
                    syncFinish("acked");
                } else {
                    syncFinish("app_abort");  // ack without ok/nack = abort
                }
                return;
            }
            if ((now - syncAckWaitStartMs) >= SYNC_ACK_TIMEOUT_MS) {
                syncFinish("ack_timeout");
            }
            return;

        case SyncState::Retx: {
            if (txBusy) break;
            if (syncRetxIdx >= syncRetxCount) {
                syncRetxCount = 0;
                syncState = SyncState::End;
                break;
            }
            if (syncResendSeq(syncRetxList[syncRetxIdx])) {
                syncLastProgressMs = now;
                syncRetxIdx++;
            }
            break;
        }
    }

    // Stall guard: a transfer that makes no progress (e.g. notifications
    // disabled mid-stream) is abandoned; nothing was marked, app re-fetches.
    if (syncState != SyncState::Idle &&
        (now - syncLastProgressMs) >= SYNC_STALL_TIMEOUT_MS) {
        syncFinish("stalled");
    }
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
        bool deferAck = false;

        // Once an accepted UNPAIR is waiting for its ACK/disconnect, freeze
        // application command handling so another operation cannot race the
        // ownership teardown.
        if (remoteUnpairState != RemoteUnpairState::Idle) {
            if (hasSeq) {
                sendCommandAck(seq, hasCmd ? cmd.c_str() : "UNKNOWN", false,
                               "UNPAIR_PENDING");
            }
            return;
        }

        if (hasCmd) {
            if (cmd == "GET_CALIBRATION_PROFILE") {
                pendingProfileListSend = true;
                ok = true;
            } else if (cmd == "TRAINING_START") {
                TrainingAlertStyle subMode = TrainingAlertStyle::Instant;
                uint32_t difficultyAngle = 0u;
                uint32_t delayMs = 0u;
                if (extractTrainingStartFields(payload, subMode, difficultyAngle, delayMs, error)) {
                    ok = startTrainingFromBle(subMode, difficultyAngle, delayMs, error);
                }
            } else if (cmd == "SET_MODE") {
                String mode;
                if (extractJsonStringField(payload, "mode", mode)) {
                    ok = applyMode(mode);
                    if (!ok) error = isCalibrating() ? "CALIBRATING" : "BAD_MODE";
                } else {
                    error = "BAD_REQUEST";
                }
            } else if (cmd == "CALIBRATE_CANCEL") {
                if (isCalibrating()) {
                    requestCalibrationCancel();
                    ok = true;
                } else {
                    error = "NOT_CALIBRATING";
                }
            } else if (cmd == "CALIBRATE_START") {
                // A "slot" field may still arrive from older apps; it is ignored —
                // profiles are addressed by id and placed automatically.
                String profileName = "Profile";
                extractJsonStringField(payload, "name", profileName);
                if (isCalibrating()) {
                    error = "ALREADY_CALIBRATING";
                } else {
                    setPendingCalibrationName(profileName.c_str());
                    requestCalibrationStart();
                    ok = true;
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
                sendTherapyPlanPacket();
                ok = true;
            } else if (cmd == "THERAPY_START") {
                uint32_t duration = 0;
                uint32_t intensity = 0;
                bool hasDuration = extractJsonIntField(payload, "therapy_duration", duration);
                bool hasIntensity = extractJsonIntField(payload, "therapy_intensity", intensity);
                if (hasDuration && hasIntensity) {
                    if ((duration == 10 || duration == 20 || duration == 30) && (intensity >= 1 && intensity <= 3)) {
                        // Duration is fixed in firmware; accept legacy app values
                        // but always run a 20-minute session.
                        therapySubModeIndex = 1;
                        therapyIntensityLevel = (int)intensity;
                        setDeviceMode(MODE_THERAPY);
                        therapyStart();
                        ok = true;
                    }
                }
            } else if (cmd == "TRAINING_STOP") {
                ok = trainingtop();
            } else if (cmd == "THERAPY_START") {
                uint32_t intensity = 0u;
                uint32_t durationMin = 0u;
                if (!extractJsonIntField(payload, "therapy_intensity", intensity) &&
                    !extractJsonIntField(payload, "intensity", intensity)) {
                    error = "BAD_REQUEST";
                } else if (!extractJsonIntField(payload, "therapy_duration", durationMin) &&
                           !extractJsonIntField(payload, "duration_min", durationMin)) {
                    error = "BAD_REQUEST";
                } else {
                    ok = startTherapyFromBle(intensity, durationMin, error);
                }
            } else if (cmd == "STOP_THERAPY" || cmd == "THERAPY_STOP" ||
                       cmd == "STOP THERAPY" || cmd == "STOP") {
                stopTherapyFromBle();
                ok = true;
            } else if (cmd == "ENTER_DFU" || cmd == "OTA_DFU" || cmd == "DFU") {
                if (!pendingDfuEnter) {
                    pendingDfuEnter = true;
                    notifyDfuStatus("ARMED");
                    // logEvent("BLE", "cmd_enter_dfu");
                }
                ok = true;
            } else if (cmd == "UNPAIR") {
                const bool verifiedOwner =
                    pairingLockState == PairingLockState::OwnerAuthorized &&
                    ownerRecordValid && connectedPeerMatchesOwner &&
                    applicationAuthorized;
                const bool busy = isCalibrating() || pendingDfuEnter;

                if (!hasSeq) {
                    error = "BAD_REQUEST";
                } else if (!verifiedOwner) {
                    error = "NOT_OWNER";
                } else if (busy || remoteUnpairState != RemoteUnpairState::Idle) {
                    error = "BUSY";
                } else {
                    remoteUnpairSeq = seq;
                    remoteUnpairQueuedMs = millis();
                    remoteUnpairState = RemoteUnpairState::AckPending;
                    deferAck = true;
                    ok = true;
                    rtt.printf("[BLE LOCK] remote unlock queued seq=%lu\n",
                               (unsigned long)seq);
                }
            } else if (cmd == "FACTORY_RESET") {
                pendingFactoryReset = true;
                ok = true;
            } else if (cmd == "FETCH_SESSIONS") {
                // Window freeze + streaming happen in bluetoothLoop(); a
                // fetch during an active transfer restarts it (nothing has
                // been marked sent yet, so no data can be lost).
                syncFetchRequested = true;
                ok = true;
            } else if (cmd == "ACK_SESSIONS") {
                uint32_t xfer = 0u;
                if (syncState != SyncState::WaitAck) {
                    error = "NOT_WAITING";
                } else if (!extractJsonIntField(payload, "xfer", xfer) ||
                           xfer != syncXferId) {
                    error = "BAD_XFER";
                } else {
                    syncRetxCount = extractJsonIntArray(payload, "nack",
                                                        syncRetxList, 16);
                    syncAckOk = (syncRetxCount == 0) &&
                                (payload.indexOf("\"ok\":true") >= 0);
                    syncAckReceived = true;
                    ok = true;
                }
            } else {
                error = "UNKNOWN_CMD";
            }

            if (hasSeq && !deferAck) {
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
        if (cmdName == "GET_CALIBRATION_PROFILE") {
            pendingProfileListSend = true;
        } else if (cmdName == "CALIBRATE_CANCEL") {
            if (isCalibrating()) {
                requestCalibrationCancel();
            }
        } else if (cmdName == "CALIBRATE_START") {
            String profileName = "Profile";
            bool autoSlot = true;
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
                    if (key == "slot") {
                        value.toUpperCase();
                        autoSlot = (value.length() == 0 || value == "AUTO");
                    }
                    if (key == "name" && value.length() > 0) profileName = value;
                }
                start = end + 1;
            }
            if (autoSlot && !isCalibrating()) {
                setPendingCalibrationName(profileName.c_str());
                requestCalibrationStart();
            }
        } else if (cmdName == "GET_THERAPY_PLAN") {
            sendTherapyPlanPacket();
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
        requestedMode.toUpperCase();
        if (requestedMode != "THERAPY") {
            applyMode(requestedMode);
        }
    }
}

static void onCharacteristicWrite(uint16_t conn_handle, BLECharacteristic *chr, uint8_t *data, uint16_t len) {
    (void)chr;
    if (data == nullptr || len == 0) return;

    // GATT encryption protects the radio packet, while this gate protects the
    // application.  Commands are accepted only after the encrypted peer has
    // also been matched to (or committed as) the persistent owner.
    if (!applicationAuthorized || conn_handle != currentConnHandle) {
        rtt.printf("[BLE LOCK] dropped RX before owner authorization h=%u\n",
                   conn_handle);
        return;
    }

    String payload;
    payload.reserve(len);
    for (uint16_t i = 0; i < len; i++) {
        payload += (char)data[i];
    }

    rttDebuggerPrintBlePacket("RX", payload.c_str());
    parseAndApplyBleCommand(payload);
}

void bluetoothSetup() {
    // rtt.print("[BLE EVT] init ");
    // rtt.println(BLE_DEVICE_NAME);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);
    turnRgbLedOff();
    batteryMonitor.begin();
    updateBatteryReading(millis());

    if (!bleInitialized) {
        Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
        Bluefruit.begin(1, 0);
        const bool supervisionConfigured =
            Bluefruit.Periph.setConnSupervisionTimeoutMS(
                BLE_PREFERRED_SUPERVISION_TIMEOUT_MS);
        Bluefruit.setEventCallback(onBleRawEvent);
        Bluefruit.autoConnLed(false); // Disable auto LED blinking on PIN_LED1/PIN_LED2 (P0.17/P0.19)
        Bluefruit.setName(BLE_DEVICE_NAME);
        Bluefruit.setTxPower(4); // dBm
        // Custom advertising below owns disconnect restarts because it must
        // select either an open policy or the controller owner whitelist.
        Bluefruit.Advertising.restartOnDisconnect(false);
        rtt.printf("[BLEL] %lu CFG dl=%u phy=%u cccd_gate=1 sup=%lu haptic=%u ppcp=%u\n",
                   millis(),
                   (unsigned)BLE_REQUEST_DATA_LENGTH_ON_CONNECT,
                   (unsigned)BLE_REQUEST_PHY_ON_CONNECT,
                   (unsigned long)BLE_PREFERRED_SUPERVISION_TIMEOUT_MS,
                   (unsigned)BLE_CONNECTION_HAPTIC_ENABLED,
                   supervisionConfigured ? 1u : 0u);
        Bluefruit.Periph.setConnectCallback(onBleConnect);
        Bluefruit.Periph.setDisconnectCallback(onBleDisconnect);
        // The pod has no display or keyboard, so use bonded Just Works pairing
        // explicitly and keep MITM disabled to match ENC_NO_MITM permissions.
        Bluefruit.Security.setIOCaps(false, false, false);
        Bluefruit.Security.setMITM(false);
        Bluefruit.Security.setPairCompleteCallback(onBlePairComplete);
        Bluefruit.Security.setSecuredCallback(onBleSecured);

        gService.begin();

        gCharacteristic.setProperties(
            CHR_PROPS_NOTIFY | CHR_PROPS_READ | CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
        gCharacteristic.setPermission(SECMODE_ENC_NO_MITM, SECMODE_ENC_NO_MITM);
        gCharacteristic.setMaxLen(512);
        gCharacteristic.setWriteCallback(onCharacteristicWrite);
        gCharacteristic.setCccdWriteCallback(onBleCccdWrite, false);
        gCharacteristic.begin();
        rttDebuggerPrintBlePacket("TX", "{}");

        pCharacteristic = &gCharacteristic;
        initializePairingLock();
        bleInitialized = true;
    }

    startAdvertising();
}

void bluetoothLoop() {
    if (!pCharacteristic) return;

    const unsigned long now = millis();
    processPairingLock(now);
    if (processRemoteUnpair(now)) {
        return;
    }
    const bool appTxAllowed = bleApplicationTxAllowed(now);

    if (pendingProfileListSend && appTxAllowed) {
        pendingProfileListSend = false;
        sendProfileList();
    }

    if (pendingDfuEnter) {
        pendingDfuEnter = false;
        // rtt.println("DFU: entering OTA bootloader");
        notifyDfuStatus("ENTERED");
        delay(50);
        enterOTADfu();
        return;
    }

    if (connected && applicationAuthorized &&
        connectionHapticPending && !connectionHapticPlayed &&
        !calibrationMotorActive() &&
        connectedSinceMs != 0UL &&
        (now - connectedSinceMs) >= CONNECTION_HAPTIC_DELAY_MS) {
        connectionHapticPending = false;
        connectionHapticPlayed = true;
        motorCancelFeedback();
        motorSetDuty(0);
        motorStartCalmHaptic(CONNECTION_HAPTIC_MS);
    }

    if (!updatePairingLed(now)) {
        updateBatteryStatusBlink(now);
    }

    // Check if telemetry state changed to set forceTelemetrySync
    char subModeStr[16];
    currentSubModeText(subModeStr, sizeof(subModeStr));

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
        lastRunning != runningNow;

    if (telemetryChanged) {
        forceTelemetrySync = true;
    }

    if (appTxAllowed) {
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

        const bool canSendBle = pCharacteristic != nullptr;

        if (canSendBle) {
            bool sentPacketThisTick = false;

            // 1. L: Always send live posture packet every 150 ms
            if (forceLiveSync ||
                lastLiveSendMs == 0 ||
                ((now - lastLiveSendMs) >= LIVE_PACKET_INTERVAL_MS)) {
                forceLiveSync = false;
                lastLiveSendMs = now;
                sendLivePacket();
                sentPacketThisTick = true;
            }

            // 2. T: Send pure training telemetry every 1000 ms in TRAINING mode, or on force sync
            if (currentMode == MODE_TRAINING &&
                (forceTelemetrySync ||
                 lastTrainingTelemetrySendMs == 0 ||
                 ((now - lastTrainingTelemetrySendMs) >= TRAINING_TELEMETRY_INTERVAL_MS))) {
                forceTelemetrySync = false;
                lastTrainingTelemetrySendMs = now;
                sendStatusTelemetry("loop", "training_1s");
                sentPacketThisTick = true;
            }

            // 3. TL: Send therapy live progress every 1000 ms in THERAPY mode
            if (currentMode == MODE_THERAPY &&
                therapyIsRunning() &&
                (lastTherapyLiveSendMs == 0 ||
                 ((now - lastTherapyLiveSendMs) >= THERAPY_LIVE_PACKET_INTERVAL_MS))) {
                lastTherapyLiveSendMs = now;
                sendTherapyLivePacket();
                sentPacketThisTick = true;
            }

            // 4. STATUS: battery + raw accelerometer axes every 5000 ms,
            // in any mode (skipped during calibration to avoid disturbing
            // the sensor sampling).
            if (!isCalibrating() &&
                (lastStatusSendMs == 0 ||
                 ((now - lastStatusSendMs) >= STATUS_PACKET_INTERVAL_MS))) {
                lastStatusSendMs = now;
                sendStatusPacket();
                sentPacketThisTick = true;
            }

            // 5. Session sync: at most one packet per tick, only on ticks
            // telemetry didn't use. Works in any mode; live packets keep
            // their cadence because they always win the tick.
            if (syncFetchRequested) {
                syncBeginTransfer();
            }
            syncProcess(now, sentPacketThisTick);
        }
    }

    if (pendingFactoryReset) {
        pendingFactoryReset = false;
        applicationAuthorized = false;
        stopPairingAdvertising();
        Bluefruit.Periph.clearBonds();
        clearOwnerRecordFiles();
        storageFactoryReset();
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
        rttDebuggerPrintBlePacket("TX", statusBuffer);
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
             "{\"dfu\":\"%s\"}",
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
    // rtt.println("[BLE EVT] start advertising");
    startAdvertising();
}

void bluetoothStopAdvertising() {
    // rtt.println("[BLE EVT] stop advertising");
    stopPairingAdvertising();
}

void bluetoothPrepareForSleep() {
    sleepShutdownRequested = true;
    bluetoothStopAdvertising();

    if (currentConnHandle != BLE_CONN_HANDLE_INVALID && Bluefruit.connected(currentConnHandle)) {
        Bluefruit.disconnect(currentConnHandle);
    } else {
        sleepShutdownRequested = false;
    }
}

bool bluetoothIsConnected() {
    return connected;
}

bool bluetoothIsAuthorized() {
    return connected && applicationAuthorized;
}

bool bluetoothPairingWindowIsOpen() {
    return pairingLockState == PairingLockState::PairingOpen ||
           pairingLockState == PairingLockState::ClaimCommitting;
}

static void beginPairingUnlock(const char* source) {
    // Persist a revocation intent first; if power is lost during the
    // transition, boot sees that marker and removes the old bond instead of
    // silently restoring it. Both physical and remote release use this path.
    batteryBlinkActive = false;
    applicationAuthorized = false;
    connectionHapticPending = false;
    connectionHapticPlayed = false;
    connectedSinceMs = 0UL;
    notificationsEnabled = false;
    applicationTxReadyLogged = false;
    stopPairingAdvertising();

    if (ownerRecordValid) {
        BleOwnerRecord revokingRecord = ownerRecord;
        revokingRecord.flags = BLE_OWNER_VALID | BLE_OWNER_REVOCATION_PENDING;
        if (!saveOwnerRecord(revokingRecord)) {
            // Removing the owner file is also crash-safe: boot treats a bond
            // with no matching owner record as invalid and clears it.
            clearOwnerRecordFiles();
        }
    }

    pairingLockState = PairingLockState::Revoking;
    pendingUnlockFinalize = true;
    preservePairingWindowOnCleanup = false;
    closePairingWindowAfterCleanup = false;
    rtt.printf("[BLE LOCK] %s unlock requested\n",
               (source && source[0] != '\0') ? source : "owner");

    if (currentConnHandle != BLE_CONN_HANDLE_INVALID && Bluefruit.connected(currentConnHandle)) {
        Bluefruit.disconnect(currentConnHandle);
        return;
    }

    // No callback will arrive when already disconnected, so the main loop can
    // finish the bond deletion and open the timed enrollment window directly.
    finalizePairingUnlock(millis());
}

void bluetoothUnlockForPairing() {
    beginPairingUnlock("physical");
}

void bluetoothClosePairingWindow() {
    pendingPairingWindowClose = true;
}

void bluetoothRequestCalibrationStart() {
    calibrationRequestStart();
}

void bluetoothRequestBatteryStatusBlink() {
    batteryBlinkActive = true;
    batteryBlinkStartMs = 0UL;
}

void bluetoothNotifyStateChanged() {
    forceTelemetrySync = true;
    forceLiveSync = true;
    telemetryCacheValid = false;
    lastLiveSendMs = 0;
    lastTrainingTelemetrySendMs = 0;
    lastTherapyLiveSendMs = 0;
}

void notifyNewSessionStored() {
    // Called from the main loop right after a promoted session lands in the
    // log. If the app is connected it gets a SESS_AVAIL hint on the next
    // idle tick and can FETCH_SESSIONS immediately (live-to-stored handoff).
    if (connected && applicationAuthorized) {
        sessAvailPending = true;
    }
}
