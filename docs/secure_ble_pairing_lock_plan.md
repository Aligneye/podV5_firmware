# Secure BLE Pairing Lock Plan

## Implementation status (October 19, 2026)

This plan is now implemented in the firmware with the following concrete
choices:

- An Idle-mode double click revokes the current owner and opens a 120-second
  pairing window.
- An unowned pod is non-connectable until that physical action opens the
  window.
- A successful encrypted bond is committed to `/ble_owner.dat` as a versioned,
  CRC-protected owner record tied to Bluefruit's stored bond.
- Locked advertising uses the nRF52 SoftDevice device-identity list and
  connection-request whitelist. This lets the owner's IRK resolve rotating
  private addresses while other phones are rejected by the controller.
- This installed Bluefruit release automatically accepts security-parameter
  requests before the raw application callback runs. The firmware therefore
  disconnects any locked-state pairing request immediately and never opens the
  application authorization gate. This also means an owner that deletes the
  phone-side bond must use the physical unlock flow before pairing again.
- Characteristic RX, notifications, telemetry, session sync, DFU commands,
  and factory-reset commands are inaccessible until the stored owner has
  restored an encrypted link.
- Unlock is power-loss-safe: a revocation-pending owner record is written
  before disconnect, then the bond and owner record are removed from the main
  loop before open advertising begins.

The custom SoftDevice advertising setup is local to `bluetooth.cpp`; no files
inside `.platformio/packages` were patched.

## Objective

Implement a single-owner BLE bond lock for the pod:

> When pairing is physically unlocked, the pod accepts one new bonded device. After that bond is committed, the pod advertises using a controller-level whitelist, accepts connections only from that bonded identity, rejects all new pairing attempts, and exposes application data only after the owner's encrypted link is restored.

The owner must be the last device successfully bonded while the physical pairing window was open. A connection by itself must never claim ownership.

Do not use the last observed MAC address or `/ble_pair.dat` as the authority. Phones commonly rotate BLE addresses. Ownership must be tied to the BLE bond's identity information and keys.

## Required security properties

The finished design must enforce these invariants:

1. The pod has at most one owner.
2. A device can become the owner only while the physical pairing window is open.
3. A successful BLE connection alone never claims ownership.
4. Ownership is committed only after:
   - pairing succeeds;
   - bonding succeeds;
   - the link is encrypted; and
   - the bond keys are confirmed in persistent storage.
5. While locked:
   - only the owner may establish a BLE connection;
   - new pairing attempts are rejected; and
   - commands, notifications, and other application traffic remain disabled until the owner's encrypted link is restored.
6. Unlocking revokes the previous owner and opens a limited pairing window.
7. Reboots, power loss, corrupt files, and incomplete ownership transitions fail closed.

## BLE terminology

### Connection

A temporary radio link between the pod and a phone. A connection alone does not prove identity and may initially be unencrypted.

### Pairing

The procedure during which two BLE devices negotiate security parameters and encryption keys.

### Bonding

Persisting the pairing keys so the encrypted relationship can be restored on later connections.

### Encryption

BLE link-layer AES-CCM protection. It protects traffic over the air after the link has been secured.

### Authentication and MITM protection

Proof that the peer is the intended peer. Just Works pairing provides encryption but does not authenticate the other device and does not protect the initial pairing against a man-in-the-middle attack.

### LTK

The Long-Term Key used to restore encryption on future connections.

### Identity address

A stable address associated with a bonded peer. This is different from the temporary address that may be visible over the air.

### RPA

A Resolvable Private Address. Phones rotate RPAs to reduce tracking.

### IRK

The Identity Resolving Key. It lets the pod recognize a bonded phone even when that phone is using a rotating RPA.

### Whitelist/filter list

A SoftDevice list controlling which peers may send scan or connection requests to the pod.

### GATT permissions

The security requirements placed on services, characteristics, and descriptors. For example, a characteristic can require an encrypted link before permitting reads or writes.

### CCCD

The Client Characteristic Configuration Descriptor through which the app enables or disables notifications.

## Pairing-lock state machine

Use an explicit state machine rather than several loosely related Boolean variables.

| State | Meaning | Advertising policy |
|---|---|---|
| `NO_OWNER_CLOSED` | No valid owner and pairing is not permitted | Stop advertising or advertise non-connectably |
| `PAIRING_OPEN` | Physical authorization was received; one new bonded device may claim ownership | General connectable advertising |
| `CLAIM_COMMITTING` | A new bond succeeded and ownership is being persisted | Stop accepting new connections |
| `OWNER_LOCKED` | A valid owner exists but is disconnected | Connectable advertising filtered to the owner |
| `OWNER_SECURING` | The owner connected but encryption has not been restored | No application traffic |
| `OWNER_AUTHORIZED` | The owner's identity is confirmed and the link is encrypted | Normal application operation |
| `REVOKING` | The previous owner and bond are being deleted | No advertising |

Primary transitions:

```text
NO_OWNER_CLOSED
      | physical unlock
      v
PAIRING_OPEN ---- timeout ----> NO_OWNER_CLOSED
      | successful bonded and encrypted pairing
      v
CLAIM_COMMITTING
      | bond verified and owner record committed
      v
OWNER_AUTHORIZED
      | disconnect
      v
OWNER_LOCKED
      | owner reconnects and restores encryption
      v
OWNER_AUTHORIZED

OWNER_LOCKED / OWNER_AUTHORIZED
      | physical unlock
      v
REVOKING ---- clear bond and whitelist ----> PAIRING_OPEN
```

The pairing window should be finite, for example 60 to 120 seconds. If it expires without a valid bond, return to `NO_OWNER_CLOSED`. Do not leave an ownerless pod pairable forever.

## Separate connection state from authorization state

The existing firmware uses `connected` for several behaviors that should require a verified owner. Replace that conceptual model with separate state:

```cpp
bool linkConnected;
bool linkSecured;
bool peerIsOwner;
bool applicationAuthorized;
```

Authorization should be derived from all required conditions:

```cpp
applicationAuthorized =
    linkConnected &&
    linkSecured &&
    peerIsOwner &&
    pairingLockState == OWNER_AUTHORIZED;
```

Require `applicationAuthorized` for:

- incoming commands;
- notifications and telemetry;
- profile access;
- session synchronization;
- factory reset;
- DFU entry;
- mode changes;
- connection-success feedback; and
- CCCD processing where practical.

The existing characteristic already requires encrypted access with `SECMODE_ENC_NO_MITM`. Keep an appropriate encrypted GATT permission, but also enforce explicit owner authorization as defense in depth.

## Persistent owner record

The existing one-byte `/ble_pair.dat` marker only says that a secured connection was observed. It does not identify the owner or prove that a usable bond remains.

Replace it with an atomic owner record such as:

```cpp
struct BleOwnerRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t generation;
    ble_gap_addr_t identityAddress;
    uint32_t bondFingerprint;
    uint32_t crc;
};
```

Suggested flags:

- `OWNER_VALID`
- `REVOCATION_PENDING`
- `CLAIM_PENDING`

The record should not duplicate the LTK unnecessarily. Bluefruit already stores peripheral bonds under `/adafruit/bond_prph`. The owner record should identify which stored bond is authoritative.

Write the record atomically:

1. Write a temporary file.
2. Flush and validate it.
3. Replace the previous record.
4. Read the result back and validate its magic, version, flags, and CRC.

### Boot reconciliation

At boot:

1. Load and validate the owner record.
2. Load the referenced Bluefruit bond.
3. Confirm that the identity address and bond agree.
4. If both are valid, enter `OWNER_LOCKED`.
5. If neither exists, enter `NO_OWNER_CLOSED`.
6. If only one exists, treat the state as inconsistent:
   - clear the orphaned record or bond;
   - log the reason;
   - fail closed; and
   - require physical unlock.
7. If `REVOCATION_PENDING` exists, finish clearing ownership before advertising.
8. If `CLAIM_PENDING` exists, verify the bond before either completing or rolling back the claim.

## Enforce the lock at the advertising layer

The old implementation continued advertising to everyone. The new implementation must enforce ownership in the SoftDevice.

### Pairing-open advertising

```text
Advertising filter = accept connection requests from anyone
Pairing requests    = permitted
```

### Owner-locked advertising

```text
SoftDevice identity list = owner identity and owner IRK
SoftDevice whitelist     = owner identity address
Advertising filter       = BLE_GAP_ADV_FP_FILTER_CONNREQ
Pairing requests          = rejected
```

The identity list allows the SoftDevice to recognize the owner when the phone presents a rotating RPA.

### SoftDevice configuration sequence

After stopping advertising and ensuring no connection is active:

1. Load the owner's `bond_keys_t`.
2. Configure `sd_ble_gap_device_identities_set()` with the peer identity and IRK.
3. Configure `sd_ble_gap_whitelist_set()` with the owner's identity address.
4. Configure advertising with `BLE_GAP_ADV_FP_FILTER_CONNREQ`.
5. Start locked advertising.

When opening pairing:

1. Stop advertising.
2. Disconnect the current link and wait for the disconnect event.
3. Clear the active whitelist.
4. Clear the device identity list.
5. Start unrestricted connectable advertising.

Every SoftDevice return code must be checked. Whitelist and identity-list changes can fail while an active BLE role is still using them.

### Required Bluefruit extension

The installed Adafruit `BLEAdvertising` implementation hardcodes:

```cpp
.filter_policy = BLE_GAP_ADV_FP_ANY
```

The clean solution is to maintain and pin a Bluefruit/framework fork that adds an API similar to:

```cpp
void BLEAdvertising::setFilterPolicy(uint8_t policy);
```

That policy must be passed into the actual SoftDevice advertising parameters.

Do not manually edit `.platformio/packages`; those edits will not be reproducible and may disappear after a package update. Pin the patched framework or library revision through the project configuration.

General-discoverable advertising flags may conflict with whitelisted advertising on this SoftDevice. Locked advertising must use compatible flags and be tested on both Android and iOS.

## Reject new pairing while locked

Whitelisted advertising is the first enforcement layer. The BLE security manager must independently enforce the same rule.

The installed Bluefruit `BLESecurity` automatically accepts `BLE_GAP_EVT_SEC_PARAMS_REQUEST`. The application's raw-event callback runs only after the library has already replied, so the current callback cannot safely reject a new pairing.

Add a library-level authorization hook:

```cpp
using pairing_request_cb_t =
    bool (*)(uint16_t connHandle,
             const ble_gap_sec_params_t* peerParams);

void setPairingRequestCallback(pairing_request_cb_t callback);
```

Inside the security event handler:

```cpp
if (stateAllowsNewPairing(connHandle)) {
    sd_ble_gap_sec_params_reply(
        connHandle,
        BLE_GAP_SEC_STATUS_SUCCESS,
        &_sec_param,
        &keyset);
} else {
    sd_ble_gap_sec_params_reply(
        connHandle,
        BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP,
        nullptr,
        nullptr);
}
```

Policy:

- `PAIRING_OPEN`: permit new pairing.
- `OWNER_LOCKED`: reject new pairing.
- `OWNER_SECURING`: permit restoration with existing keys but reject a new key exchange.
- Every other state: reject new pairing.

This prevents an owner phone that has deleted its local bond from silently creating a replacement bond while the pod remains locked. The user must physically unlock the pod before rebonding.

## Owner reconnection procedure

While locked:

1. The controller whitelist permits the owner's connection request.
2. `onBleConnect()` records the connection handle but does not start application traffic.
3. Start a security deadline, for example 10 seconds.
4. Request or allow restoration of encryption using the existing bond.
5. When the secured callback runs:
   - verify the connection security level;
   - verify that the peer resolves to the stored owner identity;
   - set `peerIsOwner = true`; and
   - set `applicationAuthorized = true`.
6. Only after authorization:
   - play connection-success feedback;
   - enable telemetry;
   - accept commands; and
   - begin session synchronization.
7. Disconnect if encryption and identity verification do not finish before the deadline.

`onBleSecured()` alone must not create a new owner. It runs after both first-time pairing and restoration of an existing bond.

## New-owner claim procedure

While `PAIRING_OPEN`:

1. An app connects.
2. Application traffic remains disabled.
3. Pairing begins.
4. Wait until both conditions are true:
   - `AUTH_STATUS` reports success with `bonded == 1`; and
   - the connection reaches the required encrypted security level.
5. Obtain the peer's stable identity address and IRK from the completed bond.
6. Enter `CLAIM_COMMITTING`.
7. Stop accepting other connections.
8. Confirm that Bluefruit's asynchronously saved bond can be loaded again.
9. Write the owner record atomically.
10. Remove any unrelated peripheral bonds.
11. Mark the current link as the authorized owner.
12. On the next disconnect, start owner-whitelisted advertising.

Never claim ownership from only:

- `BLE_GAP_EVT_CONNECTED`;
- service discovery;
- a characteristic write;
- `onBleSecured()` without successful bonding information; or
- a raw MAC address comparison.

Bluefruit saves bond keys using a deferred callback. The firmware must verify that the new bond is readable before committing the owner record.

## Unlock and revocation procedure

The chosen physical gesture should call one API:

```cpp
void pairingLockRequestUnlock();
```

The button module should not know how bonds or advertising filters are managed.

Recommended sequence:

1. Persist `REVOCATION_PENDING`.
2. Disable application authorization immediately.
3. Stop advertising.
4. If connected, request a disconnect.
5. Wait for `BLE_GAP_EVT_DISCONNECTED`.
6. Clear all peripheral bonds.
7. Delete the owner record.
8. Clear the SoftDevice whitelist and device identity list.
9. Verify that the relevant bond store is empty.
10. Clear `REVOCATION_PENDING`.
11. Enter `PAIRING_OPEN`.
12. Start the finite pairing-window timer.

Persisting revocation intent first makes the operation recoverable. If the pod resets halfway through, boot reconciliation finishes revocation before starting advertising.

## Encryption and authentication choices

### Option A: Physical window with Just Works

Keep encrypted, unauthenticated GATT permissions:

```cpp
SECMODE_ENC_NO_MITM
```

Advantages:

- BLE traffic is encrypted.
- Only the stored owner may reconnect after the pod locks.
- Mobile pairing UX is simple.

Limitation:

- Initial pairing is unauthenticated.
- A nearby attacker could race the legitimate app while the physical pairing window is open.

This may be acceptable when the physical pairing window is short and the threat model is limited to preventing casual unauthorized connections.

### Option B: Authenticated passkey pairing

Use a unique per-pod six-digit passkey:

```cpp
Bluefruit.Security.setPIN(perDevicePin);
gCharacteristic.setPermission(
    SECMODE_ENC_WITH_MITM,
    SECMODE_ENC_WITH_MITM);
```

The passkey should be randomly assigned per pod and delivered through a label, packaging, or QR code. Never use one global PIN for every pod.

The current nRF52832 Bluefruit implementation does not enable LE Secure Connections in the same manner as its CryptoCell-equipped targets. This would therefore be legacy passkey pairing. It is stronger than Just Works but is still limited by a six-digit secret.

### Stronger application-level ownership proof

For stronger initial-owner authentication, provision each pod with a random 128-bit secret and encode it in a QR code. During `PAIRING_OPEN`:

1. The pod generates a random nonce.
2. The app scans the pod's QR secret.
3. The app returns an HMAC over the nonce and pairing context.
4. The pod verifies the HMAC.
5. Only then may the completed BLE bond be committed as the owner.

This prevents a nearby phone from claiming ownership merely by winning the race during an open pairing window.

## App-side contract

### Owner reconnect

1. Scan for the pod.
2. Connect.
3. Allow the phone OS to restore the existing bond.
4. Wait for the bonded/encrypted state.
5. Enable notifications.
6. Begin normal protocol communication.

The app must not have a remote unlock command if physical access is intended to be the only way to change ownership.

### New-owner pairing

1. Ask the user to physically open pairing.
2. Scan and connect during the pairing window.
3. Initiate OS bonding.
4. If authenticated pairing is enabled, collect the per-pod PIN or QR secret.
5. Wait for bonding and encryption to complete.
6. Enable notifications.
7. Wait for the first normal pod notification before sending commands. The
   current firmware opens its application gate only after the owner record is
   durable; it does not yet define a dedicated `OWNER_COMMITTED` packet.
8. Save the pod identity in app storage. This is only for app UX and discovery;
   the phone OS's BLE bond keys, not this saved app value, prove ownership to
   the pod.

### Stale phone bond

After the pod revokes its bond, the previous phone may still retain its side of the old bond. The next connection can fail because each side has different key state.

The app must:

- recognize authentication or bond failure;
- explain that the stale system bond may need to be forgotten;
- retry pairing only after physical unlock; and
- avoid an endless automatic reconnect loop.

## Recommended code organization

`src/bluetooth.cpp` is already large. Put ownership policy in a dedicated module:

```text
include/pairing_lock.h
src/pairing_lock.cpp
```

Suggested interface:

```cpp
void pairingLockSetup();
void pairingLockLoop();

void pairingLockRequestUnlock();

bool pairingLockAllowsNewPairing(uint16_t connHandle);
bool pairingLockIsAuthorized(uint16_t connHandle);

void pairingLockOnConnected(uint16_t connHandle,
                            const ble_gap_addr_t& peer);
void pairingLockOnDisconnected(uint16_t connHandle);
void pairingLockOnAuthStatus(uint16_t connHandle,
                             uint8_t status,
                             bool bonded);
void pairingLockOnSecured(uint16_t connHandle);
```

Responsibilities:

- `pairing_lock.cpp`: ownership state, persistence, timeouts, claims, revocation, and recovery.
- `bluetooth.cpp`: BLE transport, advertising setup, and forwarding BLE events.
- `button.cpp`: request a physical unlock without containing BLE security logic.
- `config.h`: pairing-window, security-timeout, and retry constants.
- Pinned Bluefruit fork: advertising filter-policy setter, supported bond-query API, and pairing-request authorization hook.

BLE callbacks should copy small event data or set deferred flags. Filesystem work and larger state transitions should run in `pairingLockLoop()` or `bluetoothLoop()`, not in the BLE callback task.

## Factory reset, OTA, sleep, and lifecycle rules

### Factory reset

Clear all of the following as one complete operation:

- owner record;
- claim/revocation transaction state;
- Bluefruit peripheral bonds;
- SoftDevice whitelist;
- device identity list; and
- cached authorization state.

### Normal firmware update

Preserve the owner record and its corresponding bond together.

### Mass erase

Both the owner record and bonds are removed. The pod should return to `NO_OWNER_CLOSED`, not unlimited pairing mode.

### Sleep

Stop advertising but preserve ownership. On wake, reconstruct the owner identity list and whitelist before advertising.

### DFU

Verify whether the bootloader shares or ignores application bonds. Securing the application characteristic does not automatically secure a separate bootloader or DFU service. Firmware signing and DFU authorization must be reviewed separately.

### Reset during pairing

Never promote a partially saved bond to owner automatically. Use the claim transaction state and bond verification to complete or roll back deterministically.

## Failure and recovery policy

- No owner record and no bond: remain closed until physical unlock.
- Owner record exists but bond is missing: clear inconsistent ownership and fail closed.
- Bond exists but owner record is missing: clear the orphan bond and fail closed.
- Security restoration times out: disconnect and resume locked advertising.
- Pairing fails while the window remains open: allow another attempt until timeout.
- Pairing window expires during an incomplete pair: disconnect and close pairing.
- Whitelist configuration fails: do not fall back to unrestricted advertising.
- Owner-record write fails: do not report pairing success or authorize application traffic.
- Revocation fails: remain non-advertising or closed until recovery succeeds.

## Verification matrix

At minimum, test:

- first pairing from factory state;
- owner reconnect after an ordinary disconnect;
- owner reconnect after reboot;
- owner reconnect after sleep/wake;
- Android and iOS rotating private addresses;
- a second phone attempting to connect while locked;
- a second phone attempting to pair while locked;
- the owner phone deleting its local bond;
- pairing-window timeout with no connection;
- pairing completing near the window deadline;
- unlock while the owner is connected;
- power loss during revocation;
- power loss during owner commit;
- a missing owner record with an existing bond;
- an owner record with a missing or corrupt bond;
- two phones racing during `PAIRING_OPEN`;
- rejection of commands before owner authorization;
- rejection of DFU and factory-reset commands before owner authorization;
- suppression of telemetry/session sync before authorization;
- complete factory-reset removal of ownership;
- preservation of ownership across an ordinary firmware update; and
- the expected encryption level and key size on the secured connection.

## Suggested implementation order

1. Define the threat model and choose Just Works, per-device passkey, or QR-secret ownership proof.
2. Add the pairing-lock state machine and authorization gate without changing advertising yet.
3. Replace `/ble_pair.dat` with the versioned owner record and boot reconciliation.
4. Add a supported Bluefruit bond-query interface.
5. Add the Bluefruit pairing-request authorization callback.
6. Add SoftDevice identity-list and whitelist configuration.
7. Add the advertising filter-policy API and state-aware advertising controller.
8. Implement new-owner claim and verified persistence.
9. Implement transactional unlock and revocation.
10. Update every BLE command, notification, session-sync, and feedback path to require authorization.
11. Implement the app pairing/recovery states and user messaging.
12. Complete the hardware and failure-injection test matrix.

## Final architecture

The complete security boundary is:

> Single stored owner bond + peer identity/IRK resolution + SoftDevice connection whitelist + explicit pairing rejection while locked + encrypted GATT + application authorization gate + transactional physical unlock.

All of these layers are needed. A marker file, encrypted characteristic, or pairing-state Boolean by itself is not a secure ownership lock.
