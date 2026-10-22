# StopWatch Muse Gadget — Project Plan

*Prepared with Claude on Oct 16, 2026. Verified against a local clone of `facebookincubator/muse-gadget-sdk` (commit `fa073045`, Oct 15, 2026) at `~/workspace/gadgets/muse-gadget-sdk`.*

## Status (Oct 21, 2026)

The StopWatch arrived, was flashed and is paired. It's running this repo's firmware: the pinned
SDK plus `patches/0001-stopwatch-customizations.patch` and the avatar in `avatar/character/`. See the README's
Customizations section for what those change.

Done:

- [x] ESP-IDF v6.0.1 installed (`scripts/install-idf.sh`).
- [x] Factory flash backed up and checked against the chip (`backups/stopwatch-factory-20261021-152942.bin`).
- [x] Firmware built and flashed. USB serial worked from the start, so the A5 workaround wasn't needed.
- [x] SDK token set over serial. Paired in the Muse app.
- [x] **Spoken replies.** The pinned SDK only shows replies as text. The speaker was fine (the
      settings beep played), but the log showed the "reply audio" was the silence that paces the
      captions. Added Gemini TTS (`gemini-3.8-flash-lite-tts`, voice Leda; Kore and Achernar before), keyed over serial
      with `>gemini_key=`.
- [x] Fixed TLS to Google. It needed `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`
      (GTS Root R4 is cross-signed by GlobalSign).
- [x] Faster speech (Oct 22):
  - A kept TLS connection to Gemini, opened while Muse thinks.
  - Replies spoken sentence by sentence as the text streams in.
  - TLS session resumption for reconnects.
  - Text-to-first-sound went from 3.3–3.8 s to 1.0–1.5 s. Details in the README.
- [x] Voice notes ask Muse for short spoken-style answers (`MUSE_VOICE_NOTE_STYLE`). The Gemini
      connection opens on wake (red button); resumed sessions take ~50 ms.
- [x] Spoken text is cleaned of list numbers, bullets, markdown and emoji (Muse ignored "no lists").
- [x] Custom avatar: the toy astronaut, now at 384 px (6×).
- [x] Screen: Montserrat text on dark bands, bold sentence-case state word, Wi-Fi inside the battery
      ring at 3 o'clock, rim icons and top status line hidden, MOBOT painter's-tape label.
- [x] Smooth, sharper clips (Oct 22): 256 px at 24 fps, decoded with `esp_new_jpeg` in a background
      task (about 22 ms a frame, its memory in PSRAM), redrawn only where they change, scaled 3:2
      without multiplies. Measured on the watch: ticks 42 ms apart (43 ms at most), scaling 10 ms.
- [x] Listening and sending show three round dots that build up, and a sent note a tick, in place of
      "Listening 2.3s" and "Note sent - waiting for Muse". The ring is white. Pairing shows an arrow
      at the blue button with "Press to pair". The dots and tick sit on a scrim. Replies get most of the screen
      (21 x 8 pages, the character small at the top once text arrives).
- [x] **Pairing lost after flashes** (5 times on Oct 22). Every boot refreshed the token to report
      the SDK token, each refresh rotates the refresh token, and a reset (a flash, a serial monitor
      opening the port) between the server rotating it and NVS saving the new one left a stale token,
      so the next boot got HTTP 401 and unpaired. Fixed in the patch: the report happens once and is
      remembered in NVS (`sdk_tok_rptd`), the refresh isn't retried with a token the server may
      already have retired, and a failed store is logged as an error. Every flash and reset since kept
      the pairing: boots no longer refresh the token. (Opening the serial port resets the watch even
      with DTR and RTS low, but that's now harmless.)
- [x] **Spoken replies stopped (Oct 22): Gemini's free daily quota.** The TTS API answered HTTP 429,
      "limit: 100 requests per day on Tier 1"; each sentence is one request, so a reply costs 2–4.
      Fixed by adding xAI text to speech (no daily cap), used whenever `>xai_key=` is set: first
      audio about 0.33 s after the reply text, against about 1 s with Gemini. Gemini stays as the
      fallback with its key.
- [x] xAI speech tags (`[pause]`, `[laugh]`, `<whisper>`) and face tags (`[face-joy]`…) that play
      the happy, thinking, error or goodbye clip; all tags are left out of the captions. Heard on
      the watch: replies speak; whether whispers are performed is still to confirm by ear.
- [x] Clip changes fade in over three frames (125 ms) instead of cutting.
- [x] Internal RAM: four fixes (`docs/internal-ram.md`) took the lowest free internal RAM from 5–6K
      back to 18K, measured idle and through four spoken xAI replies.
- [ ] Move the SDK checkout and build folder out of iCloud-synced `~/Documents` (`SDK_DIR`).
      iCloud offloaded files mid-build and left conflict copies (`dependencies 2.lock`), which broke
      or slowed several builds on Oct 22. The xAI session already builds in `~/Library/Caches`.
- [x] Avatar as video clips (Oct 22): one clip per state, packed into a new `anims` flash
      partition and played by `avatar/character/muse_pixel.c`. The screen went black and white, and
      the state word is hidden since the clips show the state. Builds; **still to check
      on the watch** (the new partition table, the clips' frame rate and the decode time in the log).
- [x] Battery work:
  - Auto-sleep is 30 s.
  - The Wi-Fi nap starts after 30 s.
  - New Auto power-off setting (default 30 min asleep on battery).
  - CPU power-down in light sleep is left off: it needs 8.5 KB of internal RAM the board can't spare.
- [x] While asleep, red wakes the watch and holding blue (0.3 s) wakes it and listens; touch, yellow
      and a quick blue click are ignored (`.power_wakes_only`).
      Red-only wake was tried first, but with 30 s auto-sleep the talk button was usually ignored.
- [x] Blue (talk) and yellow (speaker on/off) work as mapped on the device. Red power-off and wake are still to test (below).

Decided against:

- **Thinking phrases while Muse works.** We tried them ("Hmm, let me think.", 10 random phrases),
  but they broke the flow of the conversation, so they were removed (Oct 22).

- **Mic only while talking.** The mic runs while the screen is on, into a 320 ms in-memory ring
  buffer, so the first word isn't clipped. Nothing leaves the watch until the talk button is held.
  Turning the mic off between presses saves little battery, since the screen dominates, so we kept
  the SDK's behaviour.

## Support status: confirmed ✅

Claude initially thought the StopWatch wasn't officially supported (it saw a stale README). The local clone proves it is:

- `esp32/devices/sdkconfig.muse-m5stack-stopwatch` — board overlay
- `esp32/components/muse/boards/board_m5stack_stopwatch.c` — board driver
- `esp32/tests/test_stopwatch_controls.py`, `test_stopwatch_power.py` — hardware tests
- Build table: chip `esp32s3`, overlays `sdkconfig.muse` + `sdkconfig.muse-m5stack-stopwatch`, built "by hand" (idf.py + overlays)
- Flashing: `tools/muse/board.sh flash stopwatch` — "flashing needs nothing special"

Full feature set per the support matrix: avatar status display, images from Muse, on-device UI/settings, push-to-talk, speaker + mic, touch, battery status, OTA updates, home-network tunnel. Button mapping: **blue top-right = talk, yellow top-left = speaker, red bottom-left = power**. No board port needed.

## How we'll split the work

- **Assistant (this VM):** install ESP-IDF v6.0.1, build the firmware `.bin`, read docs/AGENTS.md, prepare configs.
- **Mohamed (his Mac):** the StopWatch will be physically at home, so flashing happens on his Mac (USB serial). Either he runs the full pipeline locally with guidance, or the assistant builds the `.bin` here and he flashes it with esptool.
- **Mohamed only:** SDK token (tied to his Muse account), accepting the Gadget SDK Terms, enabling Developer mode in the Muse app.

## (A) Before the hardware arrives

**A1. Mohamed's steps**
1. Get the SDK token at `gadgets.muse.ai/settings/sdk-tokens` (signed into his own account). Keep it out of git — local untracked config or env var.
2. Read and accept the Gadget SDK Terms himself. Note: he works at Google — check employer's outside-project/open-source policy before publishing any fork publicly. Tinker privately first.
3. Enable Developer mode in the Muse app: Settings → Devices.

**A2. Toolchain: ESP-IDF v6.0.1 exactly**
```
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.0.1
cd esp-idf-v6.0.1
./install.sh esp32s3,esp32c5
. ./export.sh        # each new shell; consider an `idf601` alias
idf.py --version     # must print v6.0.1
```
Keep separate from any other IDF install; don't let a global IDF_PATH point elsewhere.

**A3. Prove the build pipeline with no hardware**
1. Build the default target: `cd esp32 && idf.py set-target esp32c5 && idf.py build`
2. Build the StopWatch overlay itself (no hardware needed to compile):
```
idf.py -B build-muse-m5stack-stopwatch -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-m5stack-stopwatch/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch" build
```
If this builds cleanly, day one is just flash + pair.

**A4. Read the board support**
- `esp32/devices/AGENTS.md` in full (board bring-up guide).
- `esp32/components/muse/boards/board_m5stack_stopwatch.c` and the stopwatch overlay — see what's already wired (display, touch, audio, buttons, power) so nothing gets re-done.

**A5. First-flash risks**
- M5Stack's factory firmware (UiFlow2-style) can take over the ESP32-S3's USB and switch off the USB serial port — the StickS3 needed a REPL workaround, and the StopWatch is a sibling design with a power-management chip. Have that workaround ready; unconfirmed until the device is plugged in.
- Back up the factory flash first (16 MB): `esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 stopwatch-factory.bin`
- NVS encryption: docs recommend `CONFIG_HOMEHUB_NVS_ENCRYPTION`, but on ESP32-S3 it can burn eFuses (irreversible) and may block returning to factory firmware. Sane path: first-light without it on a trusted network, enable later once the implications are clear.

## (B) Day-one sequence (when it arrives)

1. Unbox, charge over USB-C, note what the factory firmware does with each button.
2. Connect to the Mac; enumerate the serial port (`ls /dev/cu.usbmodem*`). If no port appears, use the A5 workaround.
3. Back up the factory flash (command above).
4. Build with the stopwatch overlay (A3).
5. Flash + monitor; watch the log for PSRAM detection (8 MB), partition table, Wi-Fi/BLE init.
6. Pair on a trusted home network (not public Wi-Fi): Muse app → Settings → Devices → Developer mode → find `MuseGadget-…`. Orange breathing = ready, blue breathing = press the physical button to confirm, green = connected.
7. Bring up features one at a time: display/avatar → touch → speaker → mic → push-to-talk → battery → OTA. Commit after each works.
8. Verify the button mapping on the real device before relying on it.
9. Decide on NVS encryption; reflash if enabling.

## First-project ideas (beyond stock push-to-talk)

The SDK lets boards expose commands Muse can call (other boards expose `sensors.read`, `camera.capture`, etc.). Natural pattern: add StopWatch-specific commands, let Muse drive them.

1. **Raise-to-talk with haptic states.** BMI270 motion detection (wrist raise / flip face-up) starts push-to-talk, button as fallback. Vibration patterns signal state (one tick = listening, two = reply incoming, long buzz = error). Mic-level ring around the AMOLED edge; black backgrounds (black pixels cost ~nothing on AMOLED — matters with 450 mAh).
2. **Voice-set timer that survives offline.** "Muse, set a 12-minute pasta timer" → new `timer.set` command. The RX8130CE RTC counts down locally even if Wi-Fi drops; sweeping arc on the round display; vibration + speaker chime at zero. Variants: Pomodoro, 2-minute routines. `timer.list` / cancel / extend.
3. **Tilt-dial navigator.** Gyroscope as a virtual watch crown: rotate to scroll through images/cards Muse pushes, tap to select. New `imu.read` command (orientation, shake-to-dismiss). Stretch: expose a Grove sensor on the HY2.0-4P port via a `sensors.read`-style command, mirroring the SenseCAP Indicator pattern.

## Still to verify

- [x] Where the SDK token gets injected: over serial (`>sdk_token=`, `main/sdk_token_console.c`)
      or with Kconfig `CONFIG_GADGET_SDK_TOKEN`. See `docs/token.md`.
- [ ] Which NVS encryption scheme `CONFIG_HOMEHUB_NVS_ENCRYPTION` uses and whether it burns eFuses.
      This matters more now, because NVS holds the Gemini key as well as the SDK token.
- [ ] Measure battery life with the new sleep settings (screen-off hours on a full charge), and
      check that Auto power-off fires on battery.
- [ ] Check the wake rules on battery: taps and yellow do nothing while asleep; red wakes the watch,
      and holding blue wakes it and listens (a click doesn't).
- [x] StopWatch board support exists — confirmed.
- [x] Build is by-hand (idf.py + overlays); flash via `tools/muse/board.sh flash stopwatch`.
