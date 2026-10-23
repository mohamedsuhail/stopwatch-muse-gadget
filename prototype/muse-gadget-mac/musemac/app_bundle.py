"""A small app bundle to run pairing in, so macOS lets it use Bluetooth.

macOS kills a process that touches CoreBluetooth unless the app responsible
for it (the terminal, an IDE, an agent) declares
NSBluetoothAlwaysUsageDescription. Launched with ``open``, this bundle is
responsible for itself: macOS asks once whether "Muse Gadget Mac" may use
Bluetooth, and remembers the answer.

The bundle is a shell script that runs this package with the current Python.
It lives in the state directory, is rebuilt on every pairing, and is signed
ad hoc so the permission sticks to its identifier.
"""

from __future__ import annotations

import os
import plistlib
import shlex
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

BUNDLE_ID = "local.musegadget.mac"
NAME = "Muse Gadget Mac"
EXECUTABLE = "musegadget-mac"
IN_APP_ENV = "MUSEMAC_IN_APP"
PID_FILE = "pair.pid"
RESULT_FILE = "pair.result"
PACKAGE_DIR = Path(__file__).resolve().parents[1]


def info_plist() -> dict:
    return {
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleName": NAME,
        "CFBundleExecutable": EXECUTABLE,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "0.1.0",
        "LSUIElement": True,   # no Dock icon
        "NSBluetoothAlwaysUsageDescription":
            "Muse Gadget Mac uses Bluetooth to pair this Mac with the Muse app.",
    }


def launcher_script(python: str = sys.executable, package_dir: Path = PACKAGE_DIR) -> str:
    env = " ".join(f"{k}={shlex.quote(v)}" for k, v in sorted(_passed_env().items()))
    return (
        "#!/bin/sh\n"
        f"cd {shlex.quote(str(package_dir))} || exit 1\n"
        f"exec env {IN_APP_ENV}=1 PYTHONPATH={shlex.quote(str(package_dir))} {env} "
        f"{shlex.quote(python)} -m musemac \"$@\"\n"
    )


def _passed_env() -> dict:
    """Where the SDK and the state are, if set: an app started by `open` doesn't inherit them."""
    from musemac import sdk

    return {k: os.environ[k] for k in (sdk.SDK_ENV, sdk.STATE_DIR_ENV) if os.environ.get(k)}


def build(directory: Path) -> Path:
    app = directory / f"{EXECUTABLE}.app"
    macos = app / "Contents" / "MacOS"
    macos.mkdir(parents=True, exist_ok=True)
    with open(app / "Contents" / "Info.plist", "wb") as f:
        plistlib.dump(info_plist(), f)
    exe = macos / EXECUTABLE
    exe.write_text(launcher_script(), encoding="utf-8")
    exe.chmod(0o700)
    subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(app)],
                   capture_output=True, check=True)
    return app


def in_app() -> bool:
    return os.environ.get(IN_APP_ENV) == "1"


def run_in_app(directory: Path, args: list[str]) -> int:
    """Runs ``python -m musemac ARGS`` inside the bundle, showing its output here."""
    app = build(directory)
    log_path = directory / "pair.log"
    log_path.write_text("", encoding="utf-8")
    os.chmod(log_path, 0o600)
    pid_path, result_path = directory / PID_FILE, directory / RESULT_FILE
    result_path.unlink(missing_ok=True)
    proc =subprocess.Popen(["/usr/bin/open", "-W", "-n", "--stdout", str(log_path),
                             "--stderr", str(log_path), str(app), "--args", *args])
    done = threading.Event()

    def follow() -> None:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            while True:
                line = f.readline()
                if line:
                    sys.stdout.write(line)
                    sys.stdout.flush()
                elif done.is_set():
                    return
                else:
                    time.sleep(0.2)

    follower = threading.Thread(target=follow, daemon=True)
    follower.start()
    try:
        proc.wait()
    except KeyboardInterrupt:
        # `open -W` only waits; the app itself has to be told to stop.
        try:
            os.kill(int(pid_path.read_text()), signal.SIGINT)
        except (OSError, ValueError):
            pass
        proc.wait()
    done.set()
    follower.join(2)
    try:
        return int(result_path.read_text())   # `open` doesn't pass on the app's exit code
    except (OSError, ValueError):
        return 1


def record_start(directory: Path) -> None:
    """Inside the app: where the outer command can find it."""
    (directory / PID_FILE).write_text(str(os.getpid()), encoding="utf-8")


def record_exit(directory: Path, code: int) -> None:
    (directory / RESULT_FILE).write_text(str(code), encoding="utf-8")
    (directory / PID_FILE).unlink(missing_ok=True)
