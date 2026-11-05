# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Development Commands

All commands use PlatformIO targeting the `nrf52832_custom` environment.

```bash
pio run -e nrf52832_custom                    # Build firmware
pio run -e nrf52832_custom -t upload          # Build and flash via OpenOCD/CMSIS-DAP
pio run -e nrf52832_custom -t clean           # Clean build artifacts
start_monitor.bat                             # Start RTT debug monitor (OpenOCD + PIO serial)
./factory_flash.ps1                           # Factory flash: mass erase → bootloader → firmware
```

Flashing uses a custom script (`upload_hooks.py`) that invokes OpenOCD with CMSIS-DAP. The firmware binary is written to `0x26000` and the signature to `0x7F000`. Build output lands in `.pio/build/nrf52832_custom/`.

## Architecture

**Platform:** Nordic nRF52832 (ARM Cortex-M4 + BLE), Arduino framework, Softdevice S132 v6.1.1.

**Main loop order** (`src/main.cpp`) is the authoritative execution sequence:
1. `buttonLoop()` — mode transitions
2. `motorUpdate()` — apply PWM from priority queue
3. `bluetoothLoop()` — BLE RX dispatch + telemetry TX
4. `calibrationLoop()` — calibration FSM
5. `therapyLoop()` — pattern execution (skipped in IDLE/training mode)
6. `trainingLoop()` — posture angle + haptic feedback (skipped in IDLE/therapy mode)
7. `maintainDeviceTime()` — auto-persist RTC epoch to flash every 30s
8. `updateSessionStats()` + `maintainSessionStats()` — session event tracking and promotion

**Mode state machine** (managed in `button.cpp`): the device boots in `IDLE`; a single click toggles `IDLE ↔ TRAINING`, a hold enters `THERAPY`, and a single click or completed session returns `THERAPY → IDLE`. Double-click starts calibration from `IDLE` or `TRAINING`; triple-click in `IDLE` enters sleep.

**Posture detection** (`src/training.cpp`): LIS3DH accelerometer sampled at 100 Hz over I2C, low-pass filtered (α=0.1), then angle computed via dot product against the stored calibration reference vector. Bad posture triggers motor feedback.

**Calibration FSM** (`src/calibration.cpp`): `IDLE → GET_READY (3s countdown) → HOLD_STILL (5s sampling)`. Samples are filtered by 2-sigma rejection; quality scored from stddev (Excellent ≥85, Good ≥70, Acceptable ≥50, Fail <50). Profiles store a 3D orientation reference vector (`refX/Y/Z`).

**Motor priority layers** (`src/motor.cpp`): base duty (therapy/training) is overridden by temporary feedback pulses (calibration success/fail), which are in turn overridden by calm haptic fade. `motorUpdate()` applies the highest-priority active layer each loop.

**Storage** (`src/storage.cpp`): LittleFS on internal nRF52 flash (28 KB at `0x6D000`, shared by all files). Settings persist via the filesystem with CRC validation. File rewrites use an atomic temp-file pattern to prevent brownout corruption. Flash files include `/sess_v2.log`, `/sess.st`, `/profiles.dat`, `/devtime.dat`.

**Session lifecycle** (`src/session_stats.cpp`): Training and therapy sessions run concurrently with independent event buffers. Sessions are only promoted (counted) after 30 seconds of activity to filter noise. At session end a self-contained `SessionRecord` (session id, type, start time, slouch periods or therapy patterns, totals) is built and handed to `session_log.cpp`.

**Session log** (`src/session_log.cpp`): append-only `/sess_v2.log` of CRC-checked records (`SREC` header … `SEND` footer with the session id) plus `SACK` sent markers. Writes are deferred ~400 ms from the main loop; RAM holds only an index. Capped at 120 records / 8 KB (oldest sent evicted first); compacted via temp file after a sync or when the file exceeds 9 KB. Legacy `/sessions.log` + `/sess_ev.dat` are migrated once at boot. Format and BLE sync protocol (`SESS_AVAIL`, `S_HDR`/`S_EV`/`S_END`): `docs/session_sync_v2.md`.

**BLE** (`src/bluetooth.cpp`): Single bidirectional characteristic (`beb5483e-...`) carries all communication with the mobile app — live telemetry (500ms interval), therapy plans, calibration results, and battery status. No separate characteristics per data type.

## Key Configuration (`include/config.h`)

All pin assignments, timing constants (debounce, mode switch delays), vibration intensity levels (0–255 PWM), therapy durations, BLE service/characteristic UUIDs, and `ALIGN_RTT_*` debug flag definitions live here. Change hardware pin mappings or behavioral thresholds in this file.

## Debug Logging

RTT (Real-Time Transfer) is used for all debug output. Enable specific channels by uncommenting flags in `include/config.h`:

| Flag | Output |
|------|--------|
| `ALIGN_RTT_JSON_LOG` | JSON packet logging |
| `ALIGN_RTT_BLE_RX_LOG` | BLE receive traffic |
| `ALIGN_RTT_STATUS_LOG` | 1-second status heartbeat |
| `ALIGN_RTT_SENSOR_LOG` | Raw accelerometer data |
| `ALIGN_RTT_CALIB_VERBOSE` | Calibration sampling detail |
| `ALIGN_RTT_THERAPY_VERBOSE` | Therapy pattern execution |
| `ALIGN_RTT_SESSION_VERBOSE` | Session state transitions |

`start_monitor.bat` opens the RTT server and pipes output to the PlatformIO serial monitor. The VS Code debugger config (`.vscode/launch.json`) uses the `.elf` at `.pio/build/nrf52832_custom/firmware.elf` with the nRF52 SVD for register inspection.
