"""pair_mac.pair() with a stand-in for the CoreBluetooth server: the SDK's setup on top of it."""

import json
import threading

from musegadget import config
from musegadget.ble_framing import ChunkAssembler
from musegadget.pair import PairOutcome

from musemac import pair_mac

TOKEN = "mgst_" + "a" * 42 + "A"


class FakeServer:
    """MacBleServer's surface: the phone writes get_device_info, then the window closes."""

    instances = []

    def __init__(self, local_name, on_write, on_disconnect):
        self.local_name = local_name
        self.on_write = on_write
        self.received = []
        self._assembler = ChunkAssembler()
        self._stopped = threading.Event()
        self._answered = threading.Event()
        FakeServer.instances.append(self)

    def mtu(self):
        return 23

    def send_packets(self, packets):
        for packet in packets:
            assert len(packet) <= 20
            message = self._assembler.feed(packet)
            if message is not None:
                self.received.append(json.loads(message))
                self._answered.set()

    def disconnect(self, delay):
        pass

    def run(self):
        self.on_write(json.dumps({"action": "get_device_info"}).encode())
        assert self._answered.wait(5)
        self.stop()

    def stop(self):
        self._stopped.set()


def test_setup_answers_the_app_as_a_community_gadget(tmp_path, monkeypatch):
    monkeypatch.setenv("MUSEGADGET_STATE_DIR", str(tmp_path))
    monkeypatch.setattr("musegadget.pair._SystemNetwork.is_online", staticmethod(lambda: True))
    opened = []
    result = pair_mac.pair(sdk_token=TOKEN, on_open=opened.append, server_class=FakeServer)
    assert result.outcome is PairOutcome.WINDOW_CLOSED
    server = FakeServer.instances[-1]
    info = server.received[-1]
    assert info["type"] == "device_info"
    assert info["model"] == "hatch_link"
    assert info["pairing_auth"] == "none"
    assert info["pairing_policy"] == "confirm_app"
    assert info["network_ready"] is True
    assert info["node_id"][len("homelink-"):].upper() == server.local_name[len("MuseGadget"):]
    assert opened[0].ble_name == server.local_name


def test_an_existing_pairing_is_kept_unless_forced(tmp_path, monkeypatch):
    monkeypatch.setenv("MUSEGADGET_STATE_DIR", str(tmp_path))
    config.save_json(config.PAIRING_FILE, {"access_token": "x", "refresh_token": "y"})
    result = pair_mac.pair(sdk_token=TOKEN, on_open=lambda w: None, server_class=FakeServer)
    assert result.outcome is PairOutcome.ALREADY_PAIRED
