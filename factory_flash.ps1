$OPENOCD = "$env:USERPROFILE\.platformio\packages\tool-openocd\bin\openocd.exe"
$SCRIPTS = "$env:USERPROFILE\.platformio\packages\tool-openocd\openocd\scripts"
$BOOTLOADER = "$env:USERPROFILE\.platformio\packages\framework-arduinoadafruitnrf52\bootloader\feather_nrf52832\feather_nrf52832_bootloader-0.9.1_s132_6.1.1.hex"

Write-Host "========================================="
Write-Host " AlignEye Pod Factory Flash"
Write-Host "========================================="

if (!(Test-Path $OPENOCD)) {
    Write-Host "ERROR: OpenOCD not found at $OPENOCD"
    exit 1
}

if (!(Test-Path $BOOTLOADER)) {
    Write-Host "ERROR: Bootloader not found at $BOOTLOADER"
    exit 1
}

Write-Host "Step 1: Mass erase..."
& $OPENOCD -s $SCRIPTS -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "init" -c "reset halt" -c "nrf5 mass_erase" -c "reset" -c "shutdown"

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Mass erase failed"
    exit 1
}

Write-Host "Step 2: Flash bootloader..."
& $OPENOCD -s $SCRIPTS -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "program {$BOOTLOADER} verify reset" -c "shutdown"

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Bootloader flash failed"
    exit 1
}

Write-Host "Step 3: Upload firmware..."
pio run -t upload

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Firmware upload failed"
    exit 1
}

Write-Host "========================================="
Write-Host " FACTORY FLASH SUCCESS"
Write-Host " Now test RGB, BLE, App connection"
Write-Host "========================================="