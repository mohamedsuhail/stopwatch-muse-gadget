# stopwatch-muse-gadget

Turn an **M5Stack StopWatch** dev kit (ESP32-S3) into a **Muse Gadget** using Meta's official [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk).

Pinned SDK commit: `fa073045a5572a8e3367fae142c1b7a7c554f222`
Toolchain: ESP-IDF v6.0.1 (exact)

> **Hardware status:** The StopWatch arrived and is running this firmware (Oct 21, 2026). Its USB
> serial port (`/dev/cu.usbmodem*`) worked out of the box, so the factory firmware needed no
> workaround. The factory flash is backed up (see step 3).

On top of the stock SDK, this repo adds:

- **Spoken replies.** At the pinned commit the SDK shows Muse's replies as text only, with no
  speech. This repo sends each reply to Google's Gemini TTS (`gemini-3.8-flash-lite-tts`, voice
  **Leda**) and plays the audio on the watch's speaker. This needs a Gemini API key (step 5).
- **A custom avatar.** A toy astronaut (`avatar/muse_pixel.c`), drawn at 448 px so it fills most of
  the round screen.
- **Battery savings.** Shorter sleep timeouts, plus an automatic power-off when the watch has been
  asleep on battery for a while.
- **Button-only wake.** While asleep, red wakes the watch, and holding blue wakes it and listens.
  Taps on the screen and a quick click of blue are ignored.

The SDK itself stays untouched in `.sdk/`. `scripts/build.sh` applies `patches/*.patch` and copies
in the avatar before every build. Details: [Customizations](#customizations).

## Quickstart

Everything is scripted. Run these on the machine where the hardware will be attached (your Mac).

```bash
chmod +x scripts/*.sh   # once per fresh clone
```

### 1. Install the toolchain

```bash
./scripts/install-idf.sh
```

Idempotent. Installs ESP-IDF v6.0.1 into `~/esp/esp-idf-v6.0.1` (esp32s3 support), separate from any other IDF install, and verifies `idf.py --version` prints `v6.0.1`. At the end it prints the export line for interactive use:

```bash
. "$HOME/esp/esp-idf-v6.0.1/export.sh"
```

### 2. Build

```bash
./scripts/build.sh
```

This does four things:

1. Clones the SDK at the pinned commit into `.sdk/muse-gadget-sdk` (override with `SDK_DIR`).
2. Applies every `patches/*.patch` to it. A patch that's already applied is skipped.
3. Copies `avatar/muse_pixel.c` into the SDK's custom-avatar slot.
4. Runs `tools/muse/board.sh build stopwatch` from the SDK's `esp32/` directory.

`board.sh build stopwatch` sets the IDF target to `esp32s3` and
`SDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch"`.
The build directory is `build-muse-m5stack-stopwatch`, and the build log says
`Custom avatar: components/muse/avatar/muse_pixel.c` when it picks up the avatar.

You don't need the SDK token or the Gemini key to build. The build only warns about the missing
token, which is expected (see `docs/token.md`).

If a patch fails to apply, the SDK checkout has local edits. `build.sh` stops and prints the
commands that discard them.

### 3. Flash

```bash
./scripts/flash.sh            # auto-detects the serial port
./scripts/flash.sh /dev/cu.usbmodem1101   # or pass it explicitly
```

**On the first flash, the script backs up the 16 MB factory firmware** to
`backups/stopwatch-factory-<date>.bin` (`esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 ...`)
and writes a `.sha256` checksum next to it before flashing. Later runs detect the existing backup and
skip straight to flashing, so you keep one golden copy of the factory image. Then it flashes with
`tools/muse/board.sh flash stopwatch [PORT]`.

The `.sha256` file only proves the file hasn't changed since it was written. To prove the backup
matches the chip, compare it against the device before the first flash:

```bash
. "$HOME/esp/esp-idf-v6.0.1/export.sh"
python -m esptool --chip esp32s3 -p PORT verify-flash 0 backups/stopwatch-factory-*.bin
```

It should report `Verification successful (digest matched)`. This repo's backup,
`backups/stopwatch-factory-20261021-152942.bin`, passed that check. Checksum it again later with:

```sh
shasum -a 256 -c backups/stopwatch-factory-*.bin.sha256   # macOS
sha256sum -c backups/stopwatch-factory-*.bin.sha256       # Linux
```

Flashing leaves the settings storage (NVS) alone. Your SDK token, Gemini key, Wi-Fi and pairing all
survive a reflash.

### 4. Add the SDK token

Pick **one** of these:

- **Recommended:** send `>sdk_token=...` over serial after flashing. The device replies
  `@sdk_token ok` and restarts.
- Or bake it in with `idf.py menuconfig` (**ESP32 Device SDK** → **Muse Gadgets SDK token**), then rebuild and reflash.

Full details, precedence rules, and the never-commit policy: `docs/token.md`.

### 5. Add the Gemini API key (spoken replies)

Without it, replies stay text captions. With it, the watch speaks them.

1. Create a key at [aistudio.google.com](https://aistudio.google.com/apikey). Give the project a
   spending limit, because the key will sit on the watch unencrypted (see Safety notes).
2. Send it over serial, the same way as the SDK token:

   ```
   >gemini_key=YOUR_KEY
   ```

   The watch replies `@gemini_key ok`. No restart is needed.

To turn spoken replies off again, send `>gemini_key=` with nothing after it. The watch replies
`@gemini_key cleared`. Full details: `docs/token.md`.

### 6. Pair

1. In the Muse app: **Settings → Devices → Developer mode**.
2. Join your trusted home Wi-Fi on both the phone and the device when prompted.
3. Hold the blue top-right button to talk. It wakes the watch too, if it's asleep.

The watch's light shows the pairing state:

| Light | Meaning |
| --- | --- |
| Orange, breathing | Ready to pair |
| Blue, breathing | Press the button to confirm |
| Green | Connected |

## Buttons

| Button | Position | Awake | Asleep |
| --- | --- | --- | --- |
| Blue | top-right | Hold to talk | Hold: wakes it and listens. Click: ignored |
| Yellow | top-left | Speaker on/off (**SPEAKER ON** / **SPEAKER OFF** on screen) | Ignored |
| Red | bottom-left | Click: power on/reset. Double-click: power off | Wakes the watch |

Touch works normally while the watch is awake. While it's asleep, taps and the yellow button are
ignored.

While the watch is asleep, a blue press shorter than 0.3 s is ignored, so it stays asleep. Hold it
and the watch wakes and listens. The microphone is off while asleep, so wait for the screen to
show it's listening before you start talking.

Don't hold red for 2 s while the USB cable is plugged in: that puts the watch into download mode
(green light flashing, screen black). To get out, unplug it, double-click red to power off, then
click red once.

## Customizations

All SDK changes are in `patches/0001-stopwatch-customizations.patch`. The avatar is in
`avatar/muse_pixel.c`.

### Spoken replies (Gemini TTS)

The pinned SDK shows Muse's replies as captions and plays silence in place of speech. Its README
leaves speech to "a text-to-speech API of your choice". The patch adds:

- **`components/muse/muse_gemini_tts.c/.h`:** speaks a reply (up to its first 1,023 characters)
  through the Gemini Interactions API, streamed, and plays the audio as it arrives.
  - One worker task speaks the sentences in order, over a single TLS connection it keeps open
    (HTTP/1.1 keep-alive). It opens that connection when you start talking, while Muse is still
    thinking. An idle connection closes after 60 s. Reconnects resume the TLS session
    (`CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS`), which makes the handshake shorter.
  - The API sends 24 kHz 16-bit mono PCM, whatever `sample_rate` the request asks for. The watch
    resamples it to the speaker's 16 kHz.
  - The model is `gemini-3.8-flash-lite-tts`, and the voice is `Leda` (`VOICE` in that file).
    You can use any of the 30 prebuilt voices, such as Kore or Puck.
  - If the request fails, the reply falls back to captions. The serial log says why.
  - Each sentence logs its timing, for example
    `sentence 1 (51 chars, kept connection): first audio 1095 ms, 4.00s of speech in 2.63s`.
- **`muse_chat_session.cpp`:** speaks a reply while its text is still streaming in.
  - The first piece goes to Gemini as soon as one sentence of at least 16 characters is complete.
  - Later pieces group sentences into about 80 characters, which means fewer requests and smoother
    speech.
  - A number's period, as in "1." or "3.5", doesn't end a sentence.
  - With the speaker off, or no key set, replies are shown as captions.
  - The turn's timing line includes `tts` (first piece sent) and `audio` (first sound), in ms
    after you let go of the button.
- **`main/muse_glue.c`:** routes the `>gemini_key=` serial command.
- **`devices/sdkconfig.muse-m5stack-stopwatch`:**
  - `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`. Google's servers present GTS Root R4
    cross-signed by GlobalSign, and without this option the watch rejects the certificate.
  - `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y`, for resumed TLS sessions.

**Shorter, spoken-style replies.** Every voice note carries an instruction in its `message`
field: answer in one to three short spoken sentences, with no lists, headings, emoji or markdown.
It's `MUSE_VOICE_NOTE_STYLE` in `components/muse/muse_chat_priv.h`. The Muse app shows the
instruction next to each voice message from the watch. Muse mostly follows it, but a question that
asks for "three facts" still gets three.

**Plain speech, whatever the format.** Muse still sometimes numbers its answers despite the
instruction, so the watch cleans the text before speaking it (`speakable()` in
`muse_chat_session.cpp`):

- List numbers ("1.", "2)") and bullets ("-", "*", "•") at the start of a line are dropped.
- Markdown marks (`*`, `_`, backticks, `#`) and emoji are removed.
- Dashes become short pauses.

The screen captions still show Muse's text as written.

**Ready on wake.** Pressing red to wake the watch opens the Gemini connection straight away. If
Wi-Fi is still rejoining after a nap, the watch waits up to 10 s for it. The handshake is then done
before you press talk. A resumed session took 54 ms, against about 2 s for a full handshake.

**Measured latency** (Oct 22, 2026, times after letting go of the talk button):

| | Before | After |
| --- | --- | --- |
| Muse's text arrives → first sound | 3.3 to 3.8 s | 1.0 to 1.5 s |
| Gemini, until its first audio | about 1.0 s | about 1.0 s, whatever the text length |
| Long replies | silent until the whole reply arrived | start after the first sentence |

Most of the remaining wait is Muse thinking (2 to 6 s after it receives the voice note), which
happens on Muse's server.

### Avatar

`avatar/muse_pixel.c` replaces the SDK's default character with a toy astronaut:

- **Look:** white helmet, orange visor rim, cyan antennae and orange suit.
- **States:** it animates boot, idle, listening, thinking, speaking, happy, error and power-off.
- **Previews:** `avatar/preview.png` shows one frame of every state. `avatar/size-options.png` shows
  the screen-size options that were compared.

To preview changes without the watch:

```bash
cd .sdk/muse-gadget-sdk/esp32
python3 tools/muse/make_gifs.py --src ../../../avatar/muse_pixel.c /tmp/avatar_gifs   # needs Pillow
```

The board file sets `.avatar_px = 448` (7× the 64 px grid). `muse_ui.c` centres a canvas that tall,
and the level meter and captions draw over its lower rows.

### Battery and sleep

| Setting | Stock SDK | This repo | Where to change it |
| --- | --- | --- | --- |
| Auto-sleep (screen off when idle) | 2 min | 30 s | Settings → Sleep |
| Wi-Fi nap (asleep on battery) | after 2 min | after 30 s | `WIFI_NAP_MS` in `muse_input.c` |
| Auto power-off (asleep on battery) | none | 30 min | Settings → Sleep: Never, 10 min, 30 min, 1 h, 4 h |

While asleep on battery, the watch also does the following (stock SDK behaviour):

- The display pauses.
- The audio rails switch off.
- The CPU drops to the crystal clock and light-sleeps.

**Wi-Fi nap.** During a Wi-Fi nap, Muse can't push anything to the watch. The first talk after
waking takes a moment while Wi-Fi reconnects.

**Auto power-off.** Auto power-off cuts the main power rail through the power-management chip.
Press the red button to turn the watch back on. It never powers off on USB power, or while a voice
note recorded offline is still waiting to send.

**Not enabled: CPU power-down in light sleep.** It would save more, but needs 8.5 KB of internal
RAM that Wi-Fi can't spare on this board. The SDK's StopWatch overlay leaves it off for that
reason.

**The mic is unchanged.** While the screen is on, or on USB power, the mic keeps the last 320 ms of
audio in a memory-only ring buffer. That way the first word isn't clipped when you press talk.
Nothing leaves the watch until you hold the blue button.

### Wake

The board file sets `.power_wakes_only = true`. While asleep, touch and the yellow button are
ignored. Red wakes the watch. Holding blue for 0.3 s wakes it and listens, and a shorter click is
ignored (`HOLD_WAKE_MS` in `muse_input.c`). A pairing prompt or an image from Muse still wakes the
screen.

## Repo layout

```
stopwatch-muse-gadget/
├── README.md             # this file
├── .gitignore            # build output, SDK checkout, sdkconfig, backups, secrets
├── scripts/
│   ├── install-idf.sh    # ESP-IDF v6.0.1 install (idempotent)
│   ├── build.sh          # clone pinned SDK, apply patches + avatar, build stopwatch firmware
│   └── flash.sh          # factory backup + flash
├── patches/
│   └── 0001-stopwatch-customizations.patch   # Gemini TTS, 448 px avatar, battery, wake
├── avatar/
│   ├── muse_pixel.c      # the astronaut avatar renderer
│   ├── preview.png       # every state, enlarged
│   └── size-options.png  # on-screen size options compared
├── docs/
│   ├── plan.md           # project plan and status
│   └── token.md          # SDK token and Gemini key: how to set them, never-commit rules
├── backups/              # factory flash backup (gitignored)
└── .sdk/                 # SDK checkout (gitignored; created by build.sh)
    └── muse-gadget-sdk/  # pinned at fa073045a5572a8e3367fae142c1b7a7c554f222, patched
```

### Changing the SDK patch

Edit the files in `.sdk/muse-gadget-sdk/`, build and test, then regenerate the patch. Mark the new
files first, or the diff leaves them out:

```bash
git -C .sdk/muse-gadget-sdk add -N esp32/components/muse/muse_gemini_tts.c esp32/components/muse/muse_gemini_tts.h
git -C .sdk/muse-gadget-sdk diff > patches/0001-stopwatch-customizations.patch
git -C .sdk/muse-gadget-sdk reset -q
```

Check the patch for keys before committing (`docs/token.md`, "Never-commit checklist").

## Safety notes

- **Back up the factory firmware before the first flash.** `scripts/flash.sh` does this
  automatically (`backups/stopwatch-factory-<date>.bin` + `.sha256`), but verify it against the chip
  (step 3) and copy the backup somewhere safe (not just the repo, and definitely not an
  unencrypted external drive). `*.bin` files are gitignored on purpose.
- **Keys never go into git.** That covers the SDK token and the Gemini key, in files, commit
  messages and pasted logs, even though this repo is private. See `docs/token.md`.
- **The Gemini key is stored on the watch unencrypted.** Anyone holding the watch could read it off
  the flash. Use a key you can revoke, on a project with a spending limit.
- **Pair only on trusted Wi-Fi.** Your home network, not café or hotel networks. The Muse app pairs
  the gadget in developer mode over the same network.
- **NVS encryption is deferred.** Enabling it can burn ESP32-S3 eFuses, which is irreversible, so
  first-light runs without it. Revisit before any long-term deployment.

## Troubleshooting

- **`idf.py` not found:** run `./scripts/install-idf.sh`, or set `IDF_EXPORT` to your v6.0.1
  `export.sh`.
- **No serial port:** plug the StopWatch in over USB-C and rerun `./scripts/flash.sh`. On macOS it
  shows up as `/dev/cu.usbmodem*` (note the `cu.` prefix). If it still doesn't appear, try another
  USB-C cable: charge-only cables have no data lines.
- **Token warning at build time:** expected. The build succeeds without a token.
- **Replies show as text but aren't spoken:**
  - **Speaker setting:** check that the screen says **SPEAKER ON** (yellow button).
  - **Gemini key:** check that the key is set (`>gemini_key=...` answers `@gemini_key ok`).
  - **Serial log:** read it for `muse_gemini_tts` lines. `HTTP 4xx` means a bad key, quota or model
    name. `Failed to verify certificate` means the cross-signed option is missing from the build's
    `sdkconfig`. Delete `build-muse-m5stack-stopwatch/` and rebuild.
- **No sound at all:** change the volume in Settings. If you hear the beep, the speaker works.
- **Patch doesn't apply:** the SDK checkout has local edits. Follow the commands `build.sh` prints,
  or delete `.sdk/` and rebuild.
- **Serial port permissions on Linux:** `pip install esptool` gives the serial port the permissions
  it needs on some setups.
