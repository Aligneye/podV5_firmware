@echo off
setlocal

set "OPENOCD=%USERPROFILE%\.platformio\packages\tool-openocd\bin\openocd.exe"
set "OPENOCD_SCRIPTS=%USERPROFILE%\.platformio\packages\tool-openocd\openocd\scripts"
set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"

echo =========================================
echo AlignEye Pod RTT Monitor
echo =========================================

if not exist "%OPENOCD%" (
    echo ERROR: OpenOCD not found:
    echo %OPENOCD%
    echo.
    echo Run this once:
    echo pio run
    pause
    exit /b 1
)

if not exist "%OPENOCD_SCRIPTS%" (
    echo ERROR: OpenOCD scripts not found:
    echo %OPENOCD_SCRIPTS%
    echo.
    echo Run this once:
    echo pio run
    pause
    exit /b 1
)

if not exist "%PIO%" (
    echo ERROR: pio.exe not found:
    echo %PIO%
    echo.
    echo Try running:
    echo python -m platformio run
    pause
    exit /b 1
)

echo [RTT] Starting OpenOCD RTT server...

start "OpenOCD RTT Server" /min "%OPENOCD%" ^
-s "%OPENOCD_SCRIPTS%" ^
-f interface/cmsis-dap.cfg ^
-f target/nrf52.cfg ^
-c "init" ^
-c "rtt setup 0x20000000 0x10000 \"SEGGER RTT\"" ^
-c "rtt start" ^
-c "rtt server start 9090 0"

echo [RTT] Waiting for RTT server to initialize...
timeout /t 3 /nobreak >nul

echo [RTT] Starting monitor...
"%PIO%" device monitor --port socket://localhost:9090 --baud 115200

endlocal