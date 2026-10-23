# This Mac as a Muse gadget

The live-voice page reaches Muse through the watch on USB. This puts the real Muse within reach of
the page without the watch: it pairs this Mac with the Muse app as a community gadget, keeps it
connected the way the SDK's Linux gadget does, and serves the watch's typed-turn protocol on
`http://127.0.0.1:8766`.

```
 live-voice page ──POST /ask──► this bridge ──/chat/stream (Noise)──► your Muse
   (localhost:8765) ◄──SSE "@chat" frames──      ◄──/chat/subscribe──
```

It is the SDK's Linux gadget (`muse-gadget-sdk/linux/src/musegadget`) imported unchanged, except
for Bluetooth: the SDK pairs over BlueZ/D-Bus (`ble_server.py`), so `musemac/ble_mac.py` serves
the same GATT service with CoreBluetooth. Everything else is the SDK's: the identity, the community
pairing, the setup commands, the "already online" Wi-Fi answer, the device tokens, the Noise
session, the reconnects, and `MuseTurn`, which picks Muse's reply out of the chat events.

Unlike the Linux gadget, the Mac registers **no commands**: Muse can talk to it but can't run
anything on it (no `system.run`, no file access).

| File | What it does |
| --- | --- |
| `musemac/sdk.py` | Finds the SDK checkout; points its state at `~/Library/Application Support/muse-gadget-mac/` |
| `musemac/keychain.py` | Reads the SDK token from the Keychain |
| `musemac/ble_mac.py` | The setup GATT service on CoreBluetooth (the SDK's setup `Transport`) |
| `musemac/app_bundle.py` | The small app pairing runs in, so macOS lets it use Bluetooth |
| `musemac/pair_mac.py` | The SDK's `pair()`, on `ble_mac` |
| `musemac/gadget.py` | The SDK's `Service`, with no commands |
| `musemac/turn.py` | One typed turn: `send_chat`, then the reply as the watch's frames |
| `musemac/bridge.py` | The HTTP bridge |

## Install

Needs macOS with Bluetooth, [uv](https://docs.astral.sh/uv/), and the SDK checkout at
`~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk` (the build's pinned clone; set
`MUSE_GADGET_SDK` to its `linux/` directory to use another). It was written against SDK commit
`fa07304`.

```bash
cd prototype/muse-gadget-mac
uv sync
```

Store your SDK token (from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens),
Account > SDK tokens) in the Keychain. `-w` last makes `security` ask for it, so it stays out of
your shell history:

```bash
security add-generic-password -s muse-sdk-token -a "$USER" -w
```

It is read from the Keychain at every start and kept in memory only; it is never written to disk,
put in the environment, or logged.

## Pair

```bash
uv run python -m musemac pair
```

This starts Bluetooth setup inside a small app, `musegadget-mac.app`, built in the state directory.
macOS kills any process that uses Bluetooth unless the app responsible for it declares why, and
terminals and agents usually don't. The first time, macOS asks whether "Muse Gadget Mac" may use
Bluetooth: allow it. The app's output shows up in your terminal.

Then, in the Muse app on your phone:

1. Turn on **Settings > Devices > Developer mode**.
2. **Settings > Devices > Add Device** (the **+** at the top right). Choose `MuseGadgetXXXXXX`, the
   name `pair` printed.
3. Muse warns that this is a community device. Continue: it's yours.
4. When it asks for Wi-Fi, pick **Use current connection**. The Mac is already online, so no
   password is needed.

`pair` prints `Paired.` and quits. Setup stays open for 10 minutes (`--timeout` to change it).
`--force` pairs again over an existing pairing. If the phone shows a GATT error, toggle the
phone's Bluetooth and try again.

State lives in `~/Library/Application Support/muse-gadget-mac/` (0700): `identity.json` is the
gadget's identity and survives unpairing, `pairing.json` holds the device tokens (0600).

```bash
uv run python -m musemac info      # node id, BLE name, paired, token present
uv run python -m musemac unpair    # forget the device tokens (remove it in the Muse app too)
```

## Run

```bash
uv run python -m musemac run
```

It connects to your Muse, logs `registered with the Muse`, and reconnects on its own. The bridge
listens on `127.0.0.1:8766` (`--port` to change it). It doesn't use Bluetooth, so it runs in any
terminal.

```bash
curl -N -X POST http://127.0.0.1:8766/ask -H 'Content-Type: application/json' -d '{"text":"What is on my calendar today?"}'
```

## The protocol

The same typed turns as the watch's `>chat=` (see `prototype/live-voice/muse-serial.js` and the
firmware's `muse_hatch_text_turn`), over HTTP.

### `POST /ask`

Body `{"text": "..."}` (up to 32 KB of UTF-8). The response is `text/event-stream`. Each event is
one frame, the JSON the watch prints after `@chat`:

```
data: {"seq":1,"type":"ack","bytes":27}
data: {"seq":2,"type":"sent","bytes":27}
data: {"seq":3,"type":"busy","on":true}
data: {"seq":4,"type":"text","msg":0,"text":"You have "}
data: {"seq":5,"type":"text","msg":0,"text":"two meetings"}
data: {"seq":6,"type":"final","msg":0,"text":"You have two meetings today."}
data: {"seq":7,"type":"message_done","msg":0,"bytes":28}
data: {"seq":8,"type":"busy","on":false}
data: {"seq":9,"type":"done","messages":1,"complete":true}
```

| Frame | Fields | Meaning |
| --- | --- | --- |
| `ack` | `bytes` | The bridge has the message (UTF-8 bytes). One per ask: the bridge takes the whole text at once, where the watch acks each `>chat+=` line |
| `sent` | `bytes` | Muse accepted the message |
| `busy` | `on` | Muse started or stopped working on it |
| `text` | `msg`, `text` | A piece of reply message `msg` (0, 1, …) |
| `final` | `msg`, `text` | Message `msg`'s whole text, sent only when it differs from the pieces; use it instead of them |
| `message_done` | `msg`, `bytes` | Message `msg` is complete |
| `done` | `messages`, `complete` | The reply is complete (`complete: false`: cut off after 15 minutes) |
| `error` | `text` | The turn ended without an answer: `MUSE NOT CONNECTED`, `MUSE REFUSED THE MESSAGE (HTTP n)`, `CAN'T SUBSCRIBE TO THE CHAT`, `NO REPLY` (none in 5 minutes), `CONNECTION LOST`, `INTERRUPTED`, `CANCELLED` |

The stream ends after `done` or `error`. `seq` counts up across turns, like the watch's console.
The page's `Reply` class in `muse-serial.js` reads these frames unchanged.

One turn at a time: a new `/ask` ends the running one with `error` `INTERRUPTED`, as a new `>chat=`
does on the watch. Closing the stream (aborting the `fetch`) ends the turn too. The message goes to
Muse's main chat, from this Mac (`device_id` is its node id), as the watch's typed turns do.

### `POST /cancel`

Ends the running turn (`>chat.cancel`): its stream gets `error` `CANCELLED`. Returns
`{"cancelled": true|false}`.

### `GET /status`

Like the watch's `>status`:

```json
{"board": "Mac (my-mac.local)", "chat": true,
 "device": {"paired": true, "connected": true, "node_id": "homelink-0707c3", "ble_name": "MuseGadget0707C3"}}
```

`device.connected` is true while the Mac is registered with Muse; asks fail with
`MUSE NOT CONNECTED` until then.

### From the page

Browsers may call it only from `http://localhost:8765`, the live-voice page's origin (CORS, with a
preflight for the JSON body). A request from any other origin, or with a `Host` other than
`127.0.0.1:8766` or `localhost:8766`, gets 403, so other sites can't ask your Muse things.
Requests with no `Origin` (curl) are allowed.

```js
const res = await fetch('http://127.0.0.1:8766/ask', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ text }), signal,
});
const reader = res.body.pipeThrough(new TextDecoderStream()).getReader();
// split on "\n\n", JSON.parse each "data: " line, feed it to Reply until "done" or "error"
```

## What's different from the Linux gadget

- **No commands.** The Linux gadget gives Muse a shell on the machine; this one doesn't.
- **The advertisement is the name alone.** The watch advertises the service UUID and the paired
  flag, with the name in a scan response. CoreBluetooth controls neither: it refuses manufacturer
  data, and with the 128-bit UUID and the name together the advertisement passes 31 bytes, and an
  Android phone (and the Muse app) never saw it. With the name alone the Muse app lists the Mac
  and pairs; the GATT service is there once the phone connects.
- **Disconnect.** A Mac peripheral can't drop a central. When setup has to drop the phone after an
  error, the service is re-published instead, which ends the phone's session the same way.
- **The app bundle.** Pairing runs inside `musegadget-mac.app` (ad-hoc signed, in the state
  directory) for the Bluetooth permission. `run` doesn't need it.
- It registers as `platform: "linux"`, `device_family: "homehub"`, like the SDK, since that is what
  the Muse server knows.

## Tests

No Bluetooth, Muse or network needed:

```bash
uv run --group dev pytest
```

`tests/test_bridge.py` runs the bridge against a fake Muse session, `tests/test_pair_mac.py` runs
the SDK's setup controller through a stand-in for the CoreBluetooth server, and `tests/test_mac.py`
covers the Keychain, the state directory, the app bundle, notification pacing and the empty command
list.
