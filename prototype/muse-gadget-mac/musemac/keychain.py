"""The SDK token, from the macOS Keychain.

You store it once, yourself:

    security add-generic-password -s muse-sdk-token -a "$USER" -w

(``-w`` last makes ``security`` ask for it, so it stays out of your shell
history.) It is read at start and handed to the SDK in memory; it is never
written to disk, put in the environment, or logged.
"""

from __future__ import annotations

import subprocess
from typing import Callable

from musegadget import config

SERVICE = "muse-sdk-token"
ADD_COMMAND = f'security add-generic-password -s {SERVICE} -a "$USER" -w'


class TokenProblem(Exception):
    pass


def _security(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(["/usr/bin/security", *args], capture_output=True, text=True,
                          timeout=10, check=False)


def sdk_token(run: Callable[[list[str]], subprocess.CompletedProcess] = _security) -> str:
    """The stored SDK token. Raises :class:`TokenProblem` when it is missing or malformed."""
    result = run(["find-generic-password", "-s", SERVICE, "-w"])
    token = result.stdout.strip() if result.returncode == 0 else ""
    if not token:
        raise TokenProblem(f"no SDK token in the Keychain; store it with: {ADD_COMMAND}")
    # The SDK's own check (config.sdk_token): an mgst_ token as gadgets.muse.ai issues them.
    if not config._SDK_TOKEN.fullmatch(token):
        raise TokenProblem("the Keychain's SDK token is not valid; copy it again from "
                           "gadgets.muse.ai and store it again")
    return token
