# Flutter Integration Guide: Secure Pod Pairing Lock

## Purpose

This document is the mobile-app contract for the pod's single-owner BLE lock.
It explains what the Flutter application must do for first-time pairing, normal
owner reconnection, ownership release, stale bonds, and error recovery.

The most important rule is:

> The Flutter app does not send an owner ID, password, MAC address, or reusable
> owner token. The phone proves ownership by successfully restoring the BLE
> bond and encrypted link that are managed by the phone OS and pod firmware.

Application storage is useful for UX and discovery, but it is never accepted
by the pod as proof of ownership.

## BLE identifiers

| Item | Value |
|---|---|
| Advertised device name | `align pod` |
| Primary service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Data characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| Characteristic properties | Read, write, write without response, notify |
| Characteristic security | Encrypted BLE link, no MITM requirement |
| Pairing-window duration | 120 seconds |
| Security timeout | 10 seconds |
| Owner-commit timeout | 5 seconds |
| Post-authorization readiness gate | Notification subscription; no fixed delay |

Use UUID matching as the primary scan filter. The advertised name is useful as
a human-readable hint, but names are not unique and must not be used as an
identity or security check.

## Security model in plain language

The pod has one of three externally relevant conditions:

1. **Unowned and closed**: it does not advertise and cannot be paired until the
   user physically opens pairing.
2. **Pairing open**: an Idle-mode double click opens a 120-second window. The
   red LED blinks and one phone may establish a new encrypted bond. Leaving
   Idle closes the window immediately.
3. **Owned and locked**: the pod advertises with a controller connection
   whitelist. Only the bonded owner's identity may connect.

After a connection, the pod still blocks all application commands and outgoing
data until the BLE link is encrypted and the connecting phone is verified as
the stored owner. A raw BLE connection is not authorization.

The pod uses the bond's Long-Term Key (LTK) to restore encryption and the
Identity Resolving Key (IRK) to recognize phones that use rotating private BLE
addresses. Flutter does not read, copy, or transmit either key.

## Required Flutter connection states

Keep the transport state separate from the product-authorization state. A
recommended app state model is:

```dart
enum PodConnectionState {
  idle,
  scanning,
  connecting,
  discoveringServices,
  securingLink,
  waitingForFirmware,
  ready,
  disconnecting,
  needsPhysicalUnlock,
  staleBond,
  failed,
}
```

Do not mark the pod as `ready` when any of these operations alone succeeds:

- scan result received;
- BLE connection established;
- services discovered;
- notification subscription requested;
- GATT write completed; or
- the OS reports that a bond exists.

Mark it `ready` only after notifications are enabled and the app receives a
valid response from the pod to a readiness probe.

## Recommended readiness probe

Use `GET_DEVICE_INFO` as a safe, repeatable readiness probe:

```json
{"seq":1,"cmd":"GET_DEVICE_INFO"}
```

Encode the JSON as UTF-8 and write it to the data characteristic. A write with
response is recommended for commands.

Once the pod's authorization gate is open, it sends:

```json
{
  "t": "INFO",
  "device_name": "...",
  "device_model": "...",
  "hw": "...",
  "fw": "...",
  "fw_build_date": "...",
  "serial": "..."
}
```

It also sends an acknowledgement when `seq` was present:

```json
{"t":"ACK","seq":1,"cmd":"GET_DEVICE_INFO","ok":true}
```

The current firmware normally sends `INFO` before `ACK`, so the app must not
assume ACK always arrives first.

The firmware opens application traffic as soon as the owner is authorized and
notifications are enabled. A command sent before authorization may be silently
discarded even if the OS reports that the GATT write succeeded. Therefore:

- subscribe to notifications first;
- allow OS pairing/encryption to finish;
- send `GET_DEVICE_INFO` as soon as notification subscription completes;
- if no `INFO` or matching ACK arrives, retry only this safe probe about once
  per second; and
- stop after a reasonable connection deadline, such as 15 seconds.

Do not retry state-changing commands as a way to test readiness.

## First-time or new-owner pairing flow

The app should present a guided pairing screen.

```text
User              Flutter app              Phone OS              Pod
 |                     |                       |                   |
 | Put pod in Idle     |                       |                   |
 | Double-click button |                       |-- pairing open -->|
 |                     |<-- scan result ---------------------------|
 |                     |-- connect ---------->|------------------->|
 |                     |-- discover/subscribe>|                   |
 |                     |                       |<-- bond/encrypt -->|
 |                     |                       |                   |-- save owner
 |                     |-- GET_DEVICE_INFO ----------------------->|
 |                     |<-------------------------- INFO + ACK ----|
 |                     |-- mark READY and store serial             |
```

Implementation steps:

1. Tell the user to place the pod in Idle mode and double-click its button.
2. Ask the user to confirm that the pod's red LED is blinking.
3. Start scanning for the service UUID.
4. If several pods are visible, show a user-selectable list with name and RSSI.
   Do not silently claim the strongest device when multiple devices are close.
5. Connect to the selected peripheral.
6. Discover the primary service and data characteristic.
7. Subscribe to notifications. The encrypted CCCD access and the firmware's
   own security request cause the OS pairing/bonding flow to run.
8. On Android, use an explicit create-bond operation if the chosen BLE package
   provides a supported one. Otherwise, accessing the protected
   characteristic should trigger system pairing.
9. On iOS, pairing is normally initiated implicitly when the app accesses the
   encrypted characteristic. There is no general-purpose app API to create or
   remove an iOS Bluetooth bond directly.
10. Allow any system Bluetooth pairing prompt to complete. This firmware uses
    Just Works pairing, so there is no pod PIN to enter.
11. Enter `waitingForFirmware` and run the readiness probe described above.
12. After receiving a valid `INFO`/ACK response, enter `ready` and permit normal
    app commands.
13. Save the returned hardware serial and the platform peripheral identifier
    for future UX and reconnect attempts.

The app must not show “paired successfully” merely because `connect()` or a
GATT write returned successfully.

## Normal owner reconnect flow

For subsequent connections:

1. Scan by service UUID and prefer the previously saved platform peripheral
   identifier when it is available.
2. Connect and discover the service/characteristic.
3. Subscribe to notifications before sending commands.
4. Let the OS restore encryption using its existing BLE bond.
5. Run the readiness probe.
6. Enter `ready` only after a valid pod response.

A normal reconnect should not show a pairing prompt. If it does, the local or
pod-side bond may have been removed or become inconsistent.

Use bounded reconnect backoff rather than a tight loop. For example, retry
after 1, 2, 5, 10, and 30 seconds while the relevant screen is active. Stop
automatic retries when the user is trying to release or transfer ownership.

## Ownership release

The verified owner may release the pod remotely. Send this only after
notifications are subscribed and the owner connection is ready:

```json
{"seq":42,"cmd":"UNPAIR"}
```

The pod replies within three seconds:

```json
{"t":"ACK","seq":42,"cmd":"UNPAIR","ok":true}
```

After a successful ACK, the app must stop automatic reconnect attempts. The
pod waits 400 ms so the notification can reach the app, disconnects, records
and completes the power-loss-safe owner/bond revocation, then opens the
120-second pairing window and blinks red. A request received during DFU or
calibration is rejected with the same ACK shape and `ok:false`; ownership is
unchanged.

Only the currently authorized encrypted owner connection can execute `UNPAIR`.
Writes from any other connection are ignored by the application authorization
gate. The command requires `seq`; firmware does not revoke ownership unless it
successfully queues the positive ACK notification.

The Idle-mode double click remains as the recovery path when the owner phone is
lost or cannot connect. It uses the same revocation and pairing-window flow,
without requiring an app ACK.

For either release path, the app should treat the resulting disconnect as
expected, stop auto-reconnecting, return to the pairing screen, and mark its
locally saved pod association as released. This local flag only controls the
UI; the pod-side revocation is what removes ownership.

## Stale bond recovery

A stale bond occurs when one side remembers old keys and the other side does
not. Common examples are:

- the pod was physically unlocked, but the old phone still stores its bond;
- the user removed the pod from Bluetooth settings but the pod is still
  locked to that phone; or
- a factory reset removed the pod-side bond.

Symptoms can include authentication failure, an immediate disconnect, a
pairing prompt on what should be a reconnect, or repeated writes with no pod
response.

Recovery procedure:

1. Stop automatic reconnect attempts.
2. Explain that both sides must forget the old relationship.
3. Ask the user to physically open pairing on the pod.
4. Remove/forget the old system bond on the phone when possible.
   - On iOS, direct the user to Bluetooth Settings and **Forget This Device**.
   - On Android, use a supported system/plugin bond-removal API if available;
     otherwise guide the user through Bluetooth Settings.
5. Scan and perform the new-owner pairing flow again.

If the phone deleted its bond but the pod remains locked, the pod will not
accept a replacement pairing from that phone—even if its address looks the
same. Physical unlock is required.

## Interpreting discovery and connection failures

The advertising packet exposes a four-byte Manufacturer Specific Data payload:

| Byte | Meaning |
|---|---|
| `0..1` | Bluetooth SIG company identifier, little-endian (`0xFFFF` during development) |
| `2` | Status format version (`1`) |
| `3` | Status flags |

Status flag bit `0` means that the pod has a verified persistent owner bond.
Bit `1` means that the physical pairing window is open. Unknown versions and
unknown flag bits must be ignored safely so the format can be extended.

The owner-bond bit does not identify the owner phone. When it is set, the app
knows only that the pod is locked to a bonded owner. The owner's connection
will pass the controller whitelist; another phone's connection will normally
time out or fail. When the pairing-open bit is set, the app may start its
guided enrollment flow.

| Observation | Likely meaning | App action |
|---|---|---|
| Pod is not found | Unowned pod is closed, pod is asleep, powered off, or out of range | For setup, ask for Idle double-click; otherwise retry scan/backoff |
| Pod is visible, owner-bond bit is set, and connection times out/fails | Pod is locked to another owner, this phone has a stale bond, or radio conditions are poor | Show owner/stale-bond guidance; ask for physical unlock only when the user intends ownership transfer |
| Pod is visible and pairing-open bit is set | The pod is accepting one new owner bond | Start the guided pairing flow before the 120-second window expires |
| Connects then disconnects quickly | New pairing was attempted while locked, or bond restoration failed | Stop retry loop and show stale-bond/ownership guidance |
| Connected but no valid notification | Encryption/owner commit is still pending, notification subscription failed, or this phone is not authorized | Retry only `GET_DEVICE_INFO` within the deadline, then disconnect and show recovery UI |
| System pairing fails | Pairing window expired, stale phone bond, or user cancelled pairing | Forget stale bond if needed, reopen pairing physically, and retry |
| Red blinking stops before setup completes | The 120-second window expired or ownership was committed | Restart the guided flow; do not continue blind writes |

## Command and notification rules

- Payloads are UTF-8 JSON strings.
- Send one logical JSON command per characteristic write.
- Prefer write-with-response for commands that change pod state.
- Subscribe before sending commands so ACK and response packets are not lost.
- Include a monotonically increasing unsigned `seq` value in JSON commands.
- Match ACK packets by both `seq` and `cmd`.
- A negative ACK currently contains `ok:false` but no detailed error string;
  handle it as a generic command rejection unless another response explains it.
- Route incoming JSON by its `t` field because telemetry can arrive between a
  command write and its ACK.
- The firmware currently does not deduplicate commands by `seq`. Retrying a
  mutating command can execute it more than once.
- Do not retry `UNPAIR` after receiving `ok:true`; the expected disconnect is
  the completion signal. If no ACK arrives within three seconds, keep the pod
  associated and show a retryable error.
- A successful GATT write means the transport accepted bytes; it does not mean
  the pod authorized or executed the command.
- Never send DFU, factory reset, calibration, therapy, or training commands
  until the readiness probe succeeds.
- Disconnect and clear pending command completers when the BLE link drops.

A minimal command helper can follow this shape:

```dart
Future<Map<String, dynamic>> sendCommand(
  String command, {
  Map<String, Object?> fields = const {},
}) async {
  if (state != PodConnectionState.ready) {
    throw StateError('Pod is not authorized and ready');
  }

  final seq = nextSequenceNumber();
  final message = <String, Object?>{
    'seq': seq,
    'cmd': command,
    ...fields,
  };

  final ackFuture = waitForMatchingAck(seq, command);
  await writeCharacteristic(
    utf8.encode(jsonEncode(message)),
    withResponse: true,
  );
  return ackFuture.timeout(const Duration(seconds: 5));
}
```

The exact scan, bonding, and characteristic APIs depend on the Flutter BLE
package selected by the app team. Keep those package-specific calls behind a
transport interface so the ownership state machine remains testable.

## Recommended local data model

Store only non-secret app metadata:

```dart
class SavedPod {
  final String serial;              // Returned by GET_DEVICE_INFO
  final String platformRemoteId;    // Android/iOS identifier used by BLE SDK
  final String displayName;
  final DateTime lastSeenAt;
  final String? firmwareVersion;
}
```

Do not store or invent:

- BLE LTK or IRK values;
- a custom owner UUID sent to the pod;
- the advertised MAC address as permanent identity;
- a global pairing PIN; or
- a Boolean that is treated as proof that this phone still owns the pod.

The returned pod serial is the product identity used by app UX. The OS bond is
the cryptographic credential used by the BLE stack.

## Platform and lifecycle requirements

- Request the Bluetooth permissions required by the supported Android/iOS
  versions before scanning or connecting.
- Handle Bluetooth-off and permission-denied states separately from
  pairing-lock failures.
- Cancel scans, notification streams, timers, and pending ACK futures when the
  owning screen/controller is disposed.
- Serialize connection attempts so two parts of the app cannot connect or
  pair the same pod simultaneously.
- Keep only one active pod connection unless product requirements explicitly
  support multiple pods.
- On app resume, re-check the actual BLE connection and readiness state rather
  than trusting the last UI state.
- App uninstall/reinstall does not necessarily remove the OS-level bond. The
  new app installation may reconnect as owner if the system bond remains.
- App local logout must not claim to revoke pod ownership. Only the physical
  pod action or an explicitly designed factory-reset workflow can do that.

## Initial-pairing security limitation

The current firmware uses BLE Just Works bonding with encryption but without
MITM authentication. The short, physically opened pairing window reduces the
attack opportunity, but another nearby phone could win the initial pairing
race.

The app must not imply that it sends a secret which prevents that race. If the
product later requires authenticated initial ownership, the protocol needs a
per-device passkey or a QR-provisioned secret with a challenge-response flow;
that would require coordinated firmware and app changes.

## App acceptance-test checklist

Before release, verify the following on both supported Android and iOS
versions:

- A closed unowned pod is not presented as ready to pair.
- Idle double-click opens setup and the red LED blinks.
- First bonding reaches `ready` and returns a stable serial.
- The same phone reconnects after disconnect, pod reboot, and app restart.
- Reconnect works while the phone uses rotating private addresses.
- A second phone cannot connect while the pod is locked.
- A second phone can become owner only after physical unlock.
- The old owner stops reconnecting after transfer.
- Removing the phone-side bond produces clear physical-unlock guidance.
- Pairing-window expiry produces a recoverable timeout screen.
- Commands are disabled until the readiness probe succeeds.
- A transport write without an ACK is not shown as command success.
- Unlock while connected produces an expected disconnect in the transfer UI.
- Factory reset removes the app's ready state and requires physical setup.
- Bluetooth-off, permission denial, out-of-range, locked-to-other-phone, and
  stale-bond failures have different user-facing messages.

## Firmware references

- Pairing lock implementation: `src/bluetooth.cpp`
- Idle double-click trigger: `src/button.cpp`
- BLE UUIDs and timeouts: `include/config.h`
- Firmware security design: `docs/secure_ble_pairing_lock_plan.md`

