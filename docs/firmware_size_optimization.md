# Firmware size optimization

Measured on 2026-10-27 with `nrf52832_custom`, Nordic nRF52 platform
10.11.0, Adafruit framework 1.7.0, and ARM GCC 7.2.1.

| Build | firmware.bin | Static RAM reported by PlatformIO |
| --- | ---: | ---: |
| Original framework `-Ofast` | 159,856 bytes | 22,348 bytes |
| Size optimization `-Os` | 119,804 bytes | 22,340 bytes |
| `-Os` plus fixed numeric date formatter | 112,988 bytes | 22,196 bytes |

The change removes the framework's `-Ofast` flag and adds `-Os` in
`platformio.ini`. Savings are 40,052 bytes (25.1%). No application source,
BLE protocol, logging, persistence format, migration, partition, or bootloader
changes were made. Floating-point formatting and debug symbols remain enabled.
LTO is not enabled.

The second pass replaces the three `strftime` calls in `device_time.cpp` with
`formatFixedTime` from `include/fixed_time_format.h`. Calendar conversion still
uses `gmtime_r`; date strings remain `YYYY-MM-DD HH:MM:SS` and
`YYYY-MM-DDTHH:MM:SSZ`. Local offsets and zero-epoch handling are unchanged.
An undersized nonempty output buffer is now explicitly set to an empty string;
the previous `strftime` call left its contents unspecified in this case.
This removes the general-purpose formatter and its dependencies, saving another
6,816 bytes and 144 bytes of static RAM. Total flash savings are 46,868 bytes
(29.3%) relative to the original image.

Validation: firmware build and signing passed. The host test compares both
formats against `strftime` at 43,201-second intervals across the uint32 epoch
range with offsets of -14, 0, and +14 hours, and checks every buffer capacity
from 0 through 32 plus null output. Pre-1970 values are skipped because the
Windows host CRT rejects them. Run with:

```
g++ -std=c++11 -static -I include tests/fixed_time_format_test.cpp -o .pio/fixed_time_format_test.exe
.pio/fixed_time_format_test.exe
```

LTO was evaluated but not retained: ARM GCC 7.2.1 failed during assembly with
an offset-out-of-range error. No LTO build-hook changes remain. The user reported
successful hardware checks for the `-Os` build; the date-formatter build still
needs device checks, especially displayed timestamps and session sync.

`-Os` also removes the nonstandard fast-math assumptions implied by `-Ofast`.
Numerical results and execution timing can consequently differ; compilation
alone cannot establish hardware equivalence. Before a production release,
check BLE pairing/reconnection and commands, calibration and posture thresholds,
training/therapy timing and haptics, session sync, persisted settings/time across
reboot, sleep/wake, and DFU entry on a development device. No device was flashed
as part of this optimization.

The smaller image improves internal dual-bank feasibility but does not enable
dual-bank OTA. Bootloader bank layout, page alignment, image-size limits, and
interrupted-update recovery must still be verified separately.

Build with `pio run -e nrf52832_custom`. To restore the previous compiler
configuration, remove `build_unflags = -Ofast` and the `-Os` build flag.
