"""``python -m musemac``: pair this Mac with Muse, then serve the local bridge.

    pair [--force] [--timeout S]   open Bluetooth setup for the Muse app
    run [--port N]                 stay connected to Muse; bridge on 127.0.0.1:8766
    info                           identity and pairing state
    unpair                         forget the device credentials (keeps the identity)
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import signal
import socket
import sys

from musegadget import config, identity
from musegadget.pair import SETUP_WINDOW_S, PairOutcome, SetupWindow

from musemac import app_bundle, keychain
from musemac.bridge import PORT, Bridge
from musemac.sdk import ensure_state_dir

log = logging.getLogger("musemac")


def _setup_open(window: SetupWindow) -> None:
    print(f"Setup open for {window.timeout_s // 60} minutes. In the Muse app: Settings > Devices >",
          flush=True)
    print(f"Developer mode on, then Add Device (+), and choose {window.ble_name}.", flush=True)


def cmd_pair(args: argparse.Namespace) -> int:
    directory = ensure_state_dir()
    try:
        token = keychain.sdk_token()
    except keychain.TokenProblem as problem:
        print(f"Can't pair: {problem}", file=sys.stderr)
        return 1
    if not app_bundle.in_app():
        if config.load_json(config.PAIRING_FILE) and not args.force:
            print("Already paired. Run `unpair` first, or pass --force.", file=sys.stderr)
            return 1
        print("Starting Bluetooth setup in the Muse Gadget Mac app (macOS may ask to allow "
              "Bluetooth the first time).", flush=True)
        forwarded = ["-v"] if args.verbose else []
        forwarded += ["pair", "--timeout", str(args.timeout)] + (["--force"] if args.force else [])
        return app_bundle.run_in_app(directory, forwarded)

    from musemac.pair_mac import pair

    app_bundle.record_start(directory)
    code = 1
    try:
        result = pair(sdk_token=token, on_open=_setup_open, force=args.force,
                      timeout_s=args.timeout)
        if result.outcome is PairOutcome.PAIRED:
            print("Paired. Start the bridge with: uv run python -m musemac run", flush=True)
            code = 0
        else:
            print({
                PairOutcome.ALREADY_PAIRED: "Already paired. Run `unpair` first, or pass --force.",
                PairOutcome.WINDOW_CLOSED: "Setup window closed without pairing.",
            }.get(result.outcome, result.outcome.value), file=sys.stderr, flush=True)
    except RuntimeError as problem:
        print(f"Can't pair: {problem}.", file=sys.stderr, flush=True)
    finally:
        app_bundle.record_exit(directory, code)
    return code


def status(service) -> dict:
    ident = service.identity
    return {
        "board": f"Mac ({socket.gethostname()})",
        "chat": True,
        "device": {
            "paired": config.load_json(config.PAIRING_FILE) is not None,
            "connected": service.current is not None,
            "node_id": ident.node_id,
            "ble_name": ident.ble_name,
        },
    }


def cmd_run(args: argparse.Namespace) -> int:
    from musemac.gadget import MacService

    ensure_state_dir()
    try:
        token = keychain.sdk_token()
    except keychain.TokenProblem as problem:
        log.warning("running without an SDK token: %s", problem)
        token = None
    if config.load_json(config.PAIRING_FILE) is None:
        log.warning("not paired yet: run `python -m musemac pair` first")

    async def main() -> None:
        service = MacService(identity=identity.load_or_create(), executor=None, sdk_token=token)
        loop = asyncio.get_running_loop()
        for signum in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(signum, service.stop)
        bridge = Bridge(session=lambda: service.current, status=lambda: status(service),
                        port=args.port)
        server = await bridge.start()
        try:
            await service.run()
        finally:
            server.close()

    asyncio.run(main())
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    ensure_state_dir()
    ident = identity.load_or_create()
    paired = config.load_json(config.PAIRING_FILE) is not None
    print(f"state:     {config.state_dir()}")
    print(f"node id:   {ident.node_id}")
    print(f"BLE name:  {ident.ble_name}")
    print(f"paired:    {'yes' if paired else 'no'}")
    try:
        keychain.sdk_token()
        print("SDK token: in the Keychain")
    except keychain.TokenProblem as problem:
        print(f"SDK token: {problem}")
    return 0


def cmd_unpair(args: argparse.Namespace) -> int:
    config.delete_json(config.PAIRING_FILE)
    print("Pairing removed; the identity is kept. Remove the device in the Muse app too.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m musemac")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    pair_parser = sub.add_parser("pair", help="open Bluetooth setup so the Muse app can pair")
    pair_parser.add_argument("--timeout", type=int, default=SETUP_WINDOW_S,
                             help="seconds to keep setup open (default: %(default)s)")
    pair_parser.add_argument("--force", action="store_true", help="pair even if already paired")
    pair_parser.set_defaults(func=cmd_pair)

    run = sub.add_parser("run", help="stay connected to Muse and serve the local bridge")
    run.add_argument("--port", type=int, default=PORT, help="bridge port (default: %(default)s)")
    run.set_defaults(func=cmd_run)

    sub.add_parser("info", help="show identity and pairing state").set_defaults(func=cmd_info)
    sub.add_parser("unpair", help="forget the saved pairing").set_defaults(func=cmd_unpair)

    args = parser.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        stream=sys.stdout if app_bundle.in_app() else sys.stderr,
    )
    return args.func(args)
