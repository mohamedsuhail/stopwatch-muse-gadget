"""Pair this Mac with the Muse app: the SDK's pair.pair(), on CoreBluetooth.

Everything but the Bluetooth server is the SDK's: the identity, the community
pairing session, the setup commands, the "already online" Wi-Fi answer, and
the check and save of the device credentials.
"""

from __future__ import annotations

import threading

from musegadget import __version__ as sdk_version
from musegadget import config, identity
from musegadget import pair as sdk_pair
from musegadget.ble_setup import SetupController
from musegadget.pair import SETUP_WINDOW_S, PairOutcome, PairResult, SetupWindow
from musegadget.pairing import PairingSession

from musemac.ble_mac import MacBleServer
from musemac.sdk import ensure_state_dir


def pair(*, sdk_token: str, on_open, force: bool = False, timeout_s: int = SETUP_WINDOW_S,
         server_class=MacBleServer) -> PairResult:
    """Open Bluetooth setup until the Muse app pairs this Mac or ``timeout_s`` passes."""
    ensure_state_dir()
    if config.load_json(config.PAIRING_FILE) and not force:
        return PairResult(PairOutcome.ALREADY_PAIRED)
    ident = identity.load_or_create()
    pairing = PairingSession(
        node_id=ident.node_id,
        device_id=ident.device_id,
        mac=ident.mac,
        firmware_version=sdk_version,
        sdk_token=sdk_token,
    )
    completed = threading.Event()
    controller: SetupController | None = None
    server = server_class(
        ident.ble_name,
        on_write=lambda packet: controller.on_write(packet),
        on_disconnect=lambda: controller.on_disconnect(),
    )

    def finish() -> None:
        completed.set()
        threading.Timer(sdk_pair.BLE_SHUTDOWN_DELAY_S, server.stop).start()

    controller = SetupController(
        pairing=pairing,
        identity=ident,
        version=sdk_version,
        transport=server,
        network=sdk_pair._SystemNetwork(),
        provision=sdk_pair._verify_and_save,
        on_complete=finish,
    )
    window = threading.Timer(timeout_s, server.stop)
    window.daemon = True

    on_open(SetupWindow(ident.ble_name, timeout_s, has_sdk_token=True))
    controller.start()
    window.start()
    try:
        server.run()
    except KeyboardInterrupt:
        pass
    finally:
        window.cancel()
        controller.stop()
    return PairResult(PairOutcome.PAIRED if completed.is_set() else PairOutcome.WINDOW_CLOSED)
