"""Where the SDK's Linux gadget lives, and this Mac's state directory.

The SDK is imported from its checkout, not copied: ``$MUSE_GADGET_SDK`` (the
``linux`` directory, or its ``src``), else the build's pinned clone. Only its
BlueZ module (``ble_server.py``) is never imported; :mod:`musemac.ble_mac`
stands in for it.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

SDK_ENV = "MUSE_GADGET_SDK"
DEFAULT_SDK = Path.home() / "Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk/linux"
# The SDK commit this was written and tested against (muse-gadget-sdk, linux/).
TESTED_SDK_COMMIT = "fa073045a5572a8e3367fae142c1b7a7c554f222"

STATE_DIR_ENV = "MUSEGADGET_STATE_DIR"   # the SDK's own variable, so config.py follows it
DEFAULT_STATE_DIR = Path.home() / "Library/Application Support/muse-gadget-mac"


def sdk_src() -> Path:
    root = Path(os.environ.get(SDK_ENV) or DEFAULT_SDK).expanduser()
    return root / "src" if (root / "src" / "musegadget").is_dir() else root


def state_dir() -> Path:
    return Path(os.environ.get(STATE_DIR_ENV) or DEFAULT_STATE_DIR)


def use() -> None:
    """Make ``import musegadget`` find the SDK, with its state in this Mac's directory."""
    src = sdk_src()
    if not (src / "musegadget" / "link_client.py").is_file():
        raise ImportError(
            f"the Muse gadget SDK isn't at {src}; set ${SDK_ENV} to its linux/ directory")
    if str(src) not in sys.path:
        sys.path.insert(0, str(src))
    os.environ.setdefault(STATE_DIR_ENV, str(DEFAULT_STATE_DIR))


def ensure_state_dir() -> Path:
    """The state directory, created if needed and kept private (0700)."""
    path = state_dir()
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(path, 0o700)
    return path
