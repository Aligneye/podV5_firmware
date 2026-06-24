import os
import sys
import subprocess
from pathlib import Path

Import("env")

def find_openocd():
    platformio_home = Path.home() / ".platformio"
    
    if sys.platform.startswith("win"):
        openocd = platformio_home / "packages" / "tool-openocd" / "bin" / "openocd.exe"
    else:
        openocd = platformio_home / "packages" / "tool-openocd" / "bin" / "openocd"

    if openocd.exists():
        return str(openocd)

    # fallback: try system PATH
    return "openocd"

def upload_firmware(source, target, env):
    openocd = find_openocd()

    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))

    firmware_bin = build_dir / "firmware.bin"
    signature_bin = build_dir / "firmware_signature.bin"

    platformio_home = Path.home() / ".platformio"
    scripts_dir = platformio_home / "packages" / "tool-openocd" / "openocd" / "scripts"

    if not firmware_bin.exists():
        raise Exception(f"Firmware file not found: {firmware_bin}")

    if not signature_bin.exists():
        raise Exception(f"Signature file not found: {signature_bin}")

    cmd = [
        openocd,
        "-s", str(scripts_dir),
        "-f", "interface/cmsis-dap.cfg",
        "-f", "target/nrf52.cfg",
        "-c", "init",
        "-c", "reset halt",
        "-c", f"program {{{firmware_bin}}} 0x26000 verify",
        "-c", f"program {{{signature_bin}}} 0x7F000 verify reset",
        "-c", "shutdown",
    ]

    print("Uploading firmware using OpenOCD...")
    print(" ".join(cmd))

    result = subprocess.run(cmd)

    if result.returncode != 0:
        raise Exception("Upload failed")

env.Replace(UPLOADCMD=upload_firmware)
