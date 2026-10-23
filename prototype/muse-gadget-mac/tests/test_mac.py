"""The Mac-specific pieces, without Bluetooth: Keychain, state, app bundle, pacing, commands."""

import plistlib
import re
import shutil
import stat
import subprocess
from types import SimpleNamespace

import pytest

from musemac import app_bundle, ble_mac, keychain, sdk
from musemac.gadget import COMMANDS, refuse_command

TOKEN = "mgst_" + "a" * 42 + "A"


def security(stdout="", returncode=0):
    calls = []

    def run(args):
        calls.append(args)
        return SimpleNamespace(stdout=stdout, returncode=returncode)
    return run, calls


def test_token_comes_from_the_keychain_item():
    run, calls = security(TOKEN + "\n")
    assert keychain.sdk_token(run) == TOKEN
    assert calls == [["find-generic-password", "-s", "muse-sdk-token", "-w"]]


@pytest.mark.parametrize("stdout,returncode", [("", 44), ("", 0), ("  \n", 0)])
def test_missing_token_says_how_to_store_it(stdout, returncode):
    run, _ = security(stdout, returncode)
    with pytest.raises(keychain.TokenProblem, match="security add-generic-password -s muse-sdk-token"):
        keychain.sdk_token(run)


def test_malformed_token_is_refused_without_echoing_it():
    secret = "mgst_not-a-real-token-but-secret"
    run, _ = security(secret)
    with pytest.raises(keychain.TokenProblem) as raised:
        keychain.sdk_token(run)
    assert secret not in str(raised.value)


def test_state_dir_is_private(tmp_path, monkeypatch):
    target = tmp_path / "state"
    target.mkdir(mode=0o755)
    monkeypatch.setenv(sdk.STATE_DIR_ENV, str(target))
    assert sdk.ensure_state_dir() == target
    assert stat.S_IMODE(target.stat().st_mode) == 0o700


def test_sdk_files_land_in_the_state_dir(tmp_path, monkeypatch):
    from musegadget import config, identity

    monkeypatch.setenv(sdk.STATE_DIR_ENV, str(tmp_path))
    ident = identity.load_or_create()
    assert re.fullmatch(r"MuseGadget[0-9A-F]{6}", ident.ble_name)
    assert stat.S_IMODE((tmp_path / config.IDENTITY_FILE).stat().st_mode) == 0o600


def test_service_and_characteristics_match_the_sdk():
    text = (sdk.sdk_src() / "musegadget" / "ble_server.py").read_text()
    for name in ("SERVICE_UUID", "RX_UUID", "TX_UUID"):
        assert f'{name} = "{getattr(ble_mac, name)}"' in text


def test_pacer_waits_for_room_and_keeps_order():
    sent, full = [], {"left": 2}

    def update(packet):
        if packet == b"b" and full["left"]:
            full["left"] -= 1
            pacer.ready()   # the delegate says there is room again
            return False
        sent.append(packet)
        return True

    pacer = ble_mac.NotifyPacer(update, sleep=lambda s: None)
    assert pacer.send([b"a", b"b", b"c"])
    assert sent == [b"a", b"b", b"c"]


def test_pacer_gives_up_on_a_phone_that_stopped_reading(monkeypatch):
    monkeypatch.setattr(ble_mac, "READY_WAIT_S", 0)
    pacer = ble_mac.NotifyPacer(lambda packet: False, sleep=lambda s: None)
    assert not pacer.send([b"a"])


def test_the_mac_gives_muse_no_commands():
    assert COMMANDS == {}
    assert refuse_command("system.run", {"command": "ls"}, None) == {
        "ok": False, "error": "system.run isn't available on this Mac"}


def test_app_bundle_declares_bluetooth_use():
    info = app_bundle.info_plist()
    assert info["NSBluetoothAlwaysUsageDescription"]
    assert info["CFBundleExecutable"] == app_bundle.EXECUTABLE
    assert info["LSUIElement"] is True


def test_launcher_runs_this_package_with_quoted_paths(tmp_path):
    script = app_bundle.launcher_script("/path with space/python", tmp_path / "pkg dir")
    assert script.startswith("#!/bin/sh\n")
    assert "'/path with space/python' -m musemac \"$@\"" in script
    assert f"cd '{tmp_path / 'pkg dir'}'" in script
    assert "MUSEMAC_IN_APP=1" in script


@pytest.mark.skipif(not shutil.which("codesign"), reason="needs macOS codesign")
def test_build_makes_a_signed_bundle(tmp_path):
    app = app_bundle.build(tmp_path)
    with open(app / "Contents" / "Info.plist", "rb") as f:
        assert plistlib.load(f)["CFBundleIdentifier"] == app_bundle.BUNDLE_ID
    assert subprocess.run(["codesign", "--verify", str(app)], capture_output=True).returncode == 0
