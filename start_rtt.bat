@echo off
echo Starting OpenOCD RTT Server on port 9090...
echo Leave this window open while you want to read the serial monitor!
echo.
set OPENOCD="%USERPROFILE%\.platformio\packages\tool-openocd\bin\openocd.exe"
set OPENOCD_SCRIPTS="%USERPROFILE%\.platformio\packages\tool-openocd\scripts"
"%OPENOCD%" -s "%OPENOCD_SCRIPTS%" -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "init" -c "rtt setup 0x20000000 0x10000 \"SEGGER RTT\"" -c "rtt start" -c "rtt server start 9090 0"
pause
