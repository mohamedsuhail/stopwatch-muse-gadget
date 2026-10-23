"""CoreBluetooth GATT peripheral for Muse Gadget setup: the macOS ble_server.py.

The same service as the SDK's BlueZ server: RX (the phone writes commands)
and TX (the device notifies responses), advertised under the gadget's BLE
name. It implements the SDK's setup ``Transport`` (``ble_setup.py``), so the
SDK's SetupController and pairing code run unchanged on top of it.

What macOS does differently from BlueZ:

* Advertising carries only the local name and service UUIDs; macOS refuses
  manufacturer data, so there is no paired flag. It is informational only.
* The peripheral learns about a central only through its requests and
  subscriptions, and cannot drop one. ``disconnect`` re-publishes the service
  instead, which ends the phone's GATT session the same way.
* A process may use Bluetooth only if the app responsible for it declares
  NSBluetoothAlwaysUsageDescription, so pairing runs inside a small app
  bundle (see :mod:`musemac.app_bundle`).

CoreBluetooth calls back on a private dispatch queue; ``send_packets`` runs
on the setup worker thread and hops onto that queue for each notification.
"""

from __future__ import annotations

import logging
import threading
import time
from typing import Callable

from musegadget.ble_framing import CHUNK_STAGGER_S, MAX_PACKET_BYTES

log = logging.getLogger(__name__)

# The SDK's ble_server.py, which can't be imported here: it needs D-Bus.
SERVICE_UUID = "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c"
RX_UUID = "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01"
TX_UUID = "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c"

_ASSUMED_MTU = MAX_PACKET_BYTES + 3
READY_WAIT_S = 1.0       # for the notify queue to drain, per try
NOTIFY_TRIES = 20


class NotifyPacer:
    """Sends packets in order, waiting whenever CoreBluetooth's notify queue is full.

    ``update(packet)`` returns False when the queue is full; ``ready()`` is
    called from the delegate once it has room again.
    """

    def __init__(self, update: Callable[[bytes], bool], sleep: Callable[[float], None] = time.sleep,
                 stagger_s: float = CHUNK_STAGGER_S) -> None:
        self._update = update
        self._sleep = sleep
        self._stagger_s = stagger_s
        self._room = threading.Event()

    def ready(self) -> None:
        self._room.set()

    def send(self, packets: list[bytes]) -> bool:
        """True if every packet went out."""
        for i, packet in enumerate(packets):
            if i:
                self._sleep(self._stagger_s)
            for _ in range(NOTIFY_TRIES):
                self._room.clear()
                if self._update(packet):
                    break
                self._room.wait(READY_WAIT_S)
            else:
                log.warning("notification dropped: the phone stopped reading")
                return False
        return True


class MacBleServer:
    """Owns a CBPeripheralManager while setup is open; implements the setup Transport."""

    def __init__(
        self,
        local_name: str,
        on_write: Callable[[bytes], None],
        on_disconnect: Callable[[], None],
    ) -> None:
        self._local_name = local_name
        self._on_write = on_write
        self._on_disconnect = on_disconnect
        self._mtu = _ASSUMED_MTU
        self._lock = threading.Lock()
        self._stopped = threading.Event()
        self._failure: str | None = None
        self._subscribers: set = set()
        self._pacer = NotifyPacer(self._update_on_queue)
        self._manager = None
        self._queue = None
        self._service = None
        self._tx = None
        self._last_value = b""

    # -- Transport --------------------------------------------------------------

    def mtu(self) -> int:
        with self._lock:
            return self._mtu

    def send_packets(self, packets: list[bytes]) -> None:
        self._pacer.send(packets)

    def disconnect(self, delay: float) -> None:
        timer = threading.Timer(delay, lambda: self._on_queue(self._republish))
        timer.daemon = True
        timer.start()

    # -- Lifecycle --------------------------------------------------------------

    def run(self) -> None:
        """Start advertising and serve until :meth:`stop`. Blocks.

        Raises RuntimeError when Bluetooth is off, unsupported or not allowed.
        """
        import CoreBluetooth as CB
        import dispatch

        self._queue = dispatch.dispatch_queue_create(b"musegadget.ble", None)
        delegate = _delegate_class().alloc().initWithServer_(self)
        self._delegate = delegate   # CoreBluetooth holds its delegate weakly
        self._manager = CB.CBPeripheralManager.alloc().initWithDelegate_queue_(delegate, self._queue)
        try:
            while not self._stopped.wait(0.5):
                pass
        finally:
            self._on_queue(self._teardown)
        if self._failure:
            raise RuntimeError(self._failure)

    def stop(self) -> None:
        self._stopped.set()

    # -- Delegate callbacks (dispatch queue) --------------------------------------

    def _state_changed(self, state: int) -> None:
        import CoreBluetooth as CB

        if state == CB.CBManagerStatePoweredOn:
            self._publish()
            return
        problem = {
            CB.CBManagerStatePoweredOff: "Bluetooth is off; turn it on and try again",
            CB.CBManagerStateUnauthorized: "this app isn't allowed to use Bluetooth "
                                           "(System Settings > Privacy & Security > Bluetooth)",
            CB.CBManagerStateUnsupported: "this Mac has no Bluetooth LE peripheral support",
        }.get(state)
        if problem:
            self._fail(problem)
        else:
            log.info("Bluetooth state %d", state)

    def _publish(self) -> None:
        import CoreBluetooth as CB

        uuid = CB.CBUUID.UUIDWithString_
        rx = CB.CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
            uuid(RX_UUID),
            CB.CBCharacteristicPropertyWrite | CB.CBCharacteristicPropertyWriteWithoutResponse,
            None, CB.CBAttributePermissionsWriteable)
        tx = CB.CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
            uuid(TX_UUID),
            CB.CBCharacteristicPropertyRead | CB.CBCharacteristicPropertyNotify,
            None, CB.CBAttributePermissionsReadable)
        service = CB.CBMutableService.alloc().initWithType_primary_(uuid(SERVICE_UUID), True)
        service.setCharacteristics_([rx, tx])
        self._service, self._tx = service, tx
        self._manager.addService_(service)

    def _service_added(self, error) -> None:
        import CoreBluetooth as CB

        if error is not None:
            self._fail(f"GATT registration failed: {error.localizedDescription()}")
            return
        log.info("GATT service registered")
        if not self._manager.isAdvertising():
            # The name alone: with the 128-bit service UUID too, the advertisement
            # passes 31 bytes and macOS sends it in a form Android phones don't
            # see (the watch splits it into a scan response; CoreBluetooth can't).
            # The service is still there once the phone connects.
            self._manager.startAdvertising_({CB.CBAdvertisementDataLocalNameKey: self._local_name})

    def _advertising_started(self, error) -> None:
        if error is not None:
            self._fail(f"advertising failed: {error.localizedDescription()}")
        else:
            log.info("advertising as %s", self._local_name)

    def _subscribed(self, central) -> None:
        with self._lock:
            self._subscribers.add(central.identifier().UUIDString())
            self._mtu = min(central.maximumUpdateValueLength(), MAX_PACKET_BYTES) + 3
            mtu = self._mtu
        log.info("client subscribed to %s (ATT MTU %d)", TX_UUID, mtu)

    def _unsubscribed(self, central) -> None:
        with self._lock:
            self._subscribers.discard(central.identifier().UUIDString())
            gone = not self._subscribers
            if gone:
                self._mtu = _ASSUMED_MTU
        if gone:
            log.info("client disconnected")
            self._on_disconnect()

    def _writes(self, requests) -> None:
        import CoreBluetooth as CB

        for request in requests:
            if request.characteristic().UUID().UUIDString().lower() != RX_UUID:
                self._manager.respondToRequest_withResult_(
                    request, CB.CBATTErrorWriteNotPermitted)
                return
        # One response answers the whole batch, as CoreBluetooth requires.
        self._manager.respondToRequest_withResult_(requests[0], CB.CBATTErrorSuccess)
        for request in requests:
            value = request.value()
            self._on_write(bytes(value) if value is not None else b"")

    def _read(self, request) -> None:
        import CoreBluetooth as CB
        from Foundation import NSData

        value = self._last_value
        offset = request.offset()
        if offset > len(value):
            self._manager.respondToRequest_withResult_(request, CB.CBATTErrorInvalidOffset)
            return
        request.setValue_(NSData.dataWithBytes_length_(value[offset:], len(value) - offset))
        self._manager.respondToRequest_withResult_(request, CB.CBATTErrorSuccess)

    # -- Internals ----------------------------------------------------------------

    def _on_queue(self, fn: Callable[[], object]):
        """Runs ``fn`` on the CoreBluetooth queue and returns its result."""
        import dispatch

        if self._queue is None:
            return None
        result = []
        dispatch.dispatch_sync(self._queue, lambda: result.append(fn()))
        return result[0] if result else None

    def _update_on_queue(self, packet: bytes) -> bool:
        from Foundation import NSData

        def update() -> bool:
            if self._tx is None:
                return True   # torn down: nothing to send to
            self._last_value = packet
            data = NSData.dataWithBytes_length_(packet, len(packet))
            return bool(self._manager.updateValue_forCharacteristic_onSubscribedCentrals_(
                data, self._tx, None))

        return bool(self._on_queue(update))

    def _republish(self) -> None:
        """The stand-in for a disconnect: the phone loses the service and starts over."""
        log.info("dropping the client: re-publishing the service")
        self._manager.removeAllServices()
        with self._lock:
            self._subscribers.clear()
            self._mtu = _ASSUMED_MTU
        self._on_disconnect()
        self._publish()

    def _fail(self, problem: str) -> None:
        log.error("%s", problem)
        self._failure = problem
        self.stop()

    def _teardown(self) -> None:
        if self._manager is not None:
            self._manager.stopAdvertising()
            self._manager.removeAllServices()
        self._tx = None


_DELEGATE = None


def _delegate_class():
    """The CBPeripheralManagerDelegate, defined once (Objective-C classes are global)."""
    global _DELEGATE
    if _DELEGATE is not None:
        return _DELEGATE
    import objc
    from Foundation import NSObject

    CBPeripheralManagerDelegate = objc.protocolNamed("CBPeripheralManagerDelegate")

    class MuseGadgetPeripheralDelegate(NSObject, protocols=[CBPeripheralManagerDelegate]):
        def initWithServer_(self, server):
            self = objc.super(MuseGadgetPeripheralDelegate, self).init()
            if self is not None:
                self.server = server
            return self

        def peripheralManagerDidUpdateState_(self, manager):
            self.server._state_changed(manager.state())

        def peripheralManager_didAddService_error_(self, manager, service, error):
            self.server._service_added(error)

        def peripheralManagerDidStartAdvertising_error_(self, manager, error):
            self.server._advertising_started(error)

        def peripheralManager_central_didSubscribeToCharacteristic_(self, manager, central, chrc):
            self.server._subscribed(central)

        def peripheralManager_central_didUnsubscribeFromCharacteristic_(self, manager, central, chrc):
            self.server._unsubscribed(central)

        def peripheralManager_didReceiveWriteRequests_(self, manager, requests):
            self.server._writes(list(requests))

        def peripheralManager_didReceiveReadRequest_(self, manager, request):
            self.server._read(request)

        def peripheralManagerIsReadyToUpdateSubscribers_(self, manager):
            self.server._pacer.ready()

    _DELEGATE = MuseGadgetPeripheralDelegate
    return _DELEGATE
