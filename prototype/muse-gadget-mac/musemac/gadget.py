"""Keep this Mac connected to its Muse: the SDK's service, with no commands.

The SDK's ``Service`` does the work (VM lookup, Noise session, token rotation,
reconnects). The Linux gadget also registers ``system.run``, ``file.read`` and
friends, which would give Muse a shell on this Mac; this one registers none,
so the Muse can talk to it but never run anything on it.
"""

from __future__ import annotations

import logging
import time

from musegadget import __version__ as sdk_version
from musegadget.link_client import DeviceDescription, LinkSession, Outcome
from musegadget.service import DEFAULT_NOISE_HOST, Service

log = logging.getLogger(__name__)

COMMANDS: dict = {}


def refuse_command(command: str, params: dict, timeout_ms: int | None) -> dict:
    return {"ok": False, "error": f"{command} isn't available on this Mac"}


class MacService(Service):
    """``Service`` with an empty command list; ``current`` is the live session, if any."""

    @property
    def current(self) -> LinkSession | None:
        session = self._current
        return session if session is not None and session.registered_at is not None else None

    async def _session(self, vm: dict, pairing: dict) -> tuple[Outcome, float]:
        # Service._session, with this Mac's commands.
        device = DeviceDescription(
            node_id=self.identity.node_id,
            display_name=self.display_name,
            version=sdk_version,
            commands=COMMANDS,
        )
        session = LinkSession(
            noise_host=pairing.get("noise_host") or DEFAULT_NOISE_HOST,
            vm_id=vm["vm_id"] or vm["vm_name"],
            vm_auth_token=vm["vm_auth_token"],
            device=device,
            run_command=refuse_command,
        )
        log.info("connecting to %s", vm["vm_name"] or vm["vm_id"])
        started = time.monotonic()
        self._current = session
        try:
            outcome = await session.run(self._stop)
        except Exception as exc:
            log.warning("session failed: %s: %s", type(exc).__name__, exc)
            outcome = Outcome.CLOSED
        finally:
            self._current = None
        lasted = time.monotonic() - (session.registered_at or time.monotonic())
        log.info("session ended: %s after %.0fs", outcome.value, time.monotonic() - started)
        return outcome, lasted
