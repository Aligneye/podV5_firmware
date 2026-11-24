# Replace ONLY the bootloader region (0x74000-0x79FFF) with the AlignEye build.
# No mass erase: app firmware, LittleFS data (sessions, profiles, settings),
# SoftDevice, MBR, UICR and the bootloader settings page (0x7F000) are kept.
#
# Why: the stock Feather nRF52832 bootloader drives P0.17 (its red LED) and
# P0.19 at boot. On the AlignEye pod P0.17 is the motor, so every power-on /
# wake made the motor buzz. This build moves the bootloader LEDs to P0.30/P0.31.
# Source: Adafruit_nRF52_Bootloader 0.9.1, board file in bootloader/board.h.

$OPENOCD = "$env:USERPROFILE\.platformio\packages\tool-openocd\bin\openocd.exe"
$SCRIPTS = "$env:USERPROFILE\.platformio\packages\tool-openocd\openocd\scripts"
$BL_BIN  = Join-Path $PSScriptRoot "bootloader\aligneye_pod_v5_bootloader-0.9.1_0x74000.bin"
$BACKUP  = Join-Path $PSScriptRoot "bootloader\backup_bootloader_0x74000.bin"

if (!(Test-Path $OPENOCD)) { Write-Host "ERROR: OpenOCD not found at $OPENOCD"; exit 1 }
if (!(Test-Path $BL_BIN))  { Write-Host "ERROR: bootloader image not found at $BL_BIN"; exit 1 }

$bl  = $BL_BIN  -replace '\\', '/'
$bak = $BACKUP  -replace '\\', '/'

Write-Host "Backing up current bootloader to $BACKUP ..."
& $OPENOCD -s $SCRIPTS -f interface/cmsis-dap.cfg -f target/nrf52.cfg `
  -c "init" -c "reset halt" -c "dump_image {$bak} 0x74000 0x6000" -c "shutdown"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: backup failed (is the pod powered on?)"; exit 1 }

Write-Host "Writing AlignEye bootloader ..."
& $OPENOCD -s $SCRIPTS -f interface/cmsis-dap.cfg -f target/nrf52.cfg `
  -c "init" -c "reset halt" `
  -c "flash write_image erase {$bl} 0x74000 bin" `
  -c "verify_image {$bl} 0x74000 bin" `
  -c "reset run" -c "shutdown"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: bootloader write failed. Restore with the backup file."; exit 1 }

Write-Host "Bootloader updated."
