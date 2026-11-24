import os
import sys
import subprocess
from pathlib import Path

Import("env")

def openocd_dir():
    # Ask PlatformIO where it installed tool-openocd (platform_packages in
    # platformio.ini), so this works on any PC / custom PLATFORMIO_CORE_DIR.
    try:
        d = env.PioPlatform().get_package_dir("tool-openocd")
        if d:
            return Path(d)
    except Exception:
        pass
    return Path.home() / ".platformio" / "packages" / "tool-openocd"

def find_openocd():
    exe = "openocd.exe" if sys.platform.startswith("win") else "openocd"
    openocd = openocd_dir() / "bin" / exe
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

    scripts_dir = openocd_dir() / "openocd" / "scripts"

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
