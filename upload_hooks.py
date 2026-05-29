import subprocess
import os

def kill_openocd(source, target, env):
    """Kill any running OpenOCD / start_rtt.bat processes before upload."""
    print("--- Closing RTT server (OpenOCD) before upload ---")
    # Kill openocd.exe so the debugger releases the probe
    subprocess.call(
        ["taskkill", "/F", "/IM", "openocd.exe"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        shell=False
    )
    # Also close any cmd window that was launched by start_rtt.bat
    # (the window title contains "start_rtt" or the script path)
    subprocess.call(
        ["taskkill", "/F", "/FI", "WINDOWTITLE eq start_rtt*"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        shell=True
    )


def start_monitor_after_upload(source, target, env):
    """Open a new Windows Terminal tab running start_monitor.bat."""
    print("--- Upload successful, launching RTT monitor in new terminal tab ---")
    bat_path = os.path.join(env.subst("$PROJECT_DIR"), "start_monitor.bat")
    subprocess.Popen(
        ["wt", "--window", "0", "new-tab", "--title", "RTT Monitor", "cmd", "/k", bat_path],
        shell=False
    )


Import("env")

env.AddPreAction("upload", kill_openocd)
env.AddPostAction("upload", start_monitor_after_upload)
