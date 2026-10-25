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
- **A custom avatar.** A toy astronaut played as video clips, one per state
  (`avatar/character/`), drawn at 384 px. The older pixel-art astronaut is still there
  (`avatar/muse_pixel.c`, `AVATAR=pixel`).
- **A cleaner screen.** Black and white throughout: white Montserrat text on soft dark bands, and
  no battery or Wi-Fi on the face (`.hide_battery_ring`; both are in Settings): at 3 o'clock, a
  bubble with the live voice's logo while it answers. There's no state word, since the clips show
  the state.
  The rim icons and the top status line are gone, and a worn white painter's-tape "MOBOT" label sits
  across the top-left.
- **Battery savings.** Shorter sleep timeouts, plus an automatic power-off when the watch has been
  asleep on battery for a while.
- **Button-only wake.** While asleep, red wakes the watch, and holding blue wakes it and listens.
  Taps on the screen and a quick click of blue are ignored.

The SDK itself is a pinned clone outside the repo, in
`~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk`. `scripts/build.sh` applies `patches/*.patch` and copies
in the avatar before every build. Details: [Customizations](#customizations).

## Quickstart

Everything is scripted. Run these on the machine where the hardware will be attached (your Mac).

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

1. Clones the SDK at the pinned commit into `~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk`
   (override with `SDK_DIR`). It's outside `~/Documents` because iCloud syncs that folder, and
   offloaded SDK files mid-build and left conflict copies of `dependencies.lock`.
2. Applies every `patches/*.patch` to it. A patch that's already applied is skipped.
3. Copies the avatar into the SDK's custom-avatar slot: `avatar/character/muse_pixel.c` (the clip
   player), or `avatar/muse_pixel.c` (the pixel astronaut) with `AVATAR=pixel ./scripts/build.sh`.
   It also copies `avatar/tape.rgb565a8` into `components/muse/tape/`.
4. Runs `tools/muse/board.sh build stopwatch` from the SDK's `esp32/` directory.

`board.sh build stopwatch` sets the IDF target to `esp32s3` and
`SDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch"`.
The build directory is `build-muse-m5stack-stopwatch`, and the build log says
`Custom avatar: components/muse/avatar/muse_pixel.c` when it picks up the avatar.

You don't need the SDK token or the Gemini key to build. The build only warns about the missing
token, which is expected (see `docs/token.md`).

If a patch fails to apply, the SDK checkout has local edits. `build.sh` stops and prints the
commands that discard them.

**One at a time.** `build.sh`, `flash.sh` and `save-patch.sh` hold a lock on the SDK checkout
(`muse-gadget-sdk.lock` next to it) while they work, because two builds in one checkout break each
other. A second one fails at once and says which script holds the lock, its process and when it
started. `LOCK_WAIT=600` makes it wait up to 10 minutes instead. A lock left by a process that's
gone is taken over.

### 3. Flash

```bash
./scripts/flash.sh            # auto-detects the serial port
./scripts/flash.sh /dev/cu.usbmodem1101   # or pass it explicitly
```

**On the first flash, the script backs up the 16 MB factory firmware** to
`backups/stopwatch-factory-<date>.bin` (`esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 ...`)
and writes a `.sha256` checksum next to it before flashing. Later runs detect the existing backup and
skip straight to flashing, so you keep one golden copy of the factory image. Then it writes the
avatar clips (`avatar/character/anims.pack`, about 9.7 MB) to the `anims` partition (`0x630000`,
read from the SDK's partition table, refusing a pack that doesn't fit it),
skipping them when the chip already has them (`esptool verify-flash`), and flashes the firmware with
`tools/muse/board.sh flash stopwatch [PORT]`. The clips go first and leave the chip in its bootloader,
so the firmware flash resets it only once. A second reset a moment into booting can cut off the
watch's login refresh after Muse's server has issued a new token but before the watch has saved it,
and the next boot then unpairs it.

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

**Or use xAI instead.** Gemini's TTS model allows 100 requests a day on Tier 1, and each reply takes
a few. xAI's text to speech has no daily cap (it limits requests per second) and costs $15 per
million characters. Create a key at [console.x.ai](https://console.x.ai) (it needs prepaid credit),
then send `>xai_key=YOUR_KEY`. The watch replies `@xai_key ok`. While an xAI key is stored, every
reply is spoken by xAI; send `>xai_key=` to clear it and go back to Gemini.

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
| Blue | top-right | Hold to talk (a click shows an arrow at it: "Hold to talk") | Hold: wakes it and listens. Click: ignored |
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
`avatar/character/` (the clips), with the pixel astronaut in `avatar/muse_pixel.c`.

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
- **`components/muse/muse_xai_tts.c/.h`:** the same job through xAI's streaming text to speech
  (`wss://api.x.ai/v1/tts`), used instead of Gemini whenever an xAI key is stored.
  - A reply is one utterance on a WebSocket the worker keeps open: each piece goes in as a
    `text.delta`, the end as `text.done`, and audio streams back until `audio.done`. A cancelled
    reply closes the connection, which is the quickest way to stop the audio still coming.
  - It asks for 16-bit mono PCM at 16 kHz, the speaker's rate, so nothing is resampled.
  - The voice is `iris` (`VOICE` in that file); any of xAI's voices, or a custom voice ID, works.
  - Only the provider in use opens a connection, so the two never hold TLS at once.
  - While an xAI key is set, each message to Muse ends with a short hint: talk like a person, use
    xAI's speech tags (`[laugh]`, `[pause]`, `<whisper>…</whisper>`…) where they fit, and pick a face
    (`[face-joy]`, `[face-thinking]`, `[face-sad]`, `[face-goodnight]`). Face tags play the happy,
    thinking, error or goodbye clip over the speaking one (`muse_state_react()`, `clip_for()` in
    `avatar/character/muse_pixel.c`) and are cut out before the voice. The captions leave every tag
    out. A wrapping tag is never split across the pieces sent to xAI.
  - **How a reply ends:** with its last face, played once more as the watch goes idle (a sad
    answer ends sad, not on a hop), then the idle clip. A reply with no face just settles back to
    idle. Finishing a reply never plays the tap reaction.
  - Each reply logs its timing, as
    `reply (N sentences, N chars, kept connection): first audio N ms, Ns of speech in Ns`.
- **`main/muse_glue.c`:** routes the `>gemini_key=` and `>xai_key=` serial commands.
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

The avatar is a toy astronaut (`avatar/character/astronaut.jpg`) played as video clips, one per
state. `avatar/character/muse_pixel.c` replaces the SDK's pixel renderer behind the same
`muse_pixel.h` API, so the UI around it is unchanged.

- **Clips:** `avatar/character/clips/` holds the `.mp4`s (16:9, the character centred on black),
  made from the prompts in `avatar/character/ANIMATION_PROMPTS.md`: all but wake and error start
  and end on one neutral front pose, so any clip cuts into any other. Listening is also the idle
  loop; every two loops of it while idle, bubbles or juggling plays once, in turn. While Muse
  works on a question in a conversation, a nod, then checking (a wrist tap). Speaking, thinking,
  happy (`[face-joy]`, a hop), sad (a head shake) and goodbye (a wave into sleep, cut at 3.8 s to
  hold asleep) as before. One clip changing into another goes through a poof: a white ring
  flashing out from the character with eight sparkles over 0.3 s, over the three-frame fade
  (Mobot's puff of magic smoke, without the smoke: LVGL circles, nothing stored). Waking is the
  set's stretch and yawn, held on its neutral end, and error
  is the head shake looped: every clip is from the one set now, so none cuts into another with a
  jump (the old wake and error clips, drawn apart, did).
- **Tapping the character** waves back: goodbye's wave (0.7 s in, for 1.5 s), held on its last
  frame until the tap reaction ends (`CLIP_WAVE` in `avatar/character/muse_pixel.c`).
- **Packing:** `python3 avatar/character/make_anims.py` (needs ffmpeg and Pillow,
  `avatar/requirements.txt`) crops each clip to its centre square at 256 px at the clip's own
  frame rate (speaking 24 fps; joy, sad and thinking 15; nod, goodnight and wake 12; the calm
  loops 8-10), makes the near-black background true black, sharpens it a little, scales each to
  the listening clip's size, and cuts still frames off the one-shots. It writes
  `avatar/character/anims.pack` (version 2: a clip count and each clip's fps; baseline JPEGs at
  quality 85 plus an index, 9.70 MB) and refuses one that won't fit the 9.81 MB partition; the
  format is in the script's docstring. All 13 clips at 24 fps would need 14 MB. `avatar/quality-options.png` compares the sizes and qualities tried.
- **On the watch:** the pack lives in its own `anims` flash partition
  (`esp32/partitions_stopwatch.csv`: 3 MB app slots, then `anims` from `0x630000` to the end of the
  16 MB flash, 9.81 MB; the firmware is about 2.5 MB). Without the partition, or before `flash.sh` has written it, the avatar stays
  black and the log says why.
- **Playback:** the UI ticks every 42 ms (`.frame_ms = 42`, 24 Hz), one tick per frame. A
  background task decodes the next frame with Espressif's SIMD JPEG decoder (`esp_new_jpeg`, about
  22 ms a frame) into a second buffer, so the tick only swaps buffers. The decoder's stack and its
  tables are in PSRAM (the clip player supplies `esp_new_jpeg`'s allocators, preferring PSRAM),
  so it takes no internal RAM, which Wi-Fi, TLS and BLE run short of. `docs/internal-ram.md` has
  where the rest goes and the fixes that keep the low point near 18K. The decoder also finds the
  box of pixels that changed, and only that box is redrawn (`muse_pixel_dirty()`, a hook the patch
  adds to `muse_pixel.h`; other avatars keep the UI's own per-cell check). The 256 px frame is
  scaled 3:2 to the 384 px canvas by repeating and averaging pixels (each pair a, b becomes
  a, (a+b)/2, b), which needs no multiplies.
- **Clip changes:** the new clip fades in over its first three frames (125 ms), blended 3/4, 1/2
  and 1/4 with a still of the last frame shown. The decoder copies that still into a third 128 KB
  PSRAM buffer at the change and blends it into each decoded frame, two pixels at a time with the
  same no-multiply averaging. Fading from the old clip still moving would take two decodes a frame,
  more than the 42 ms tick. Starting the new clip at its best-matching frame instead was tried and
  barely helps, since the clips' poses differ too much.
- **Timing:** the log reports decode and tick times once after boot (`muse_anim: ... frames decode
  in ...`, which includes any blending, and `... ticks at ... ms apart`).
- **Preview:** `avatar/character/preview.gif` shows every clip.

**The pixel astronaut.** `avatar/muse_pixel.c` draws the same character as 64 px pixel art (white
helmet, orange visor rim, cyan antennae, orange suit). Build it with `AVATAR=pixel
./scripts/build.sh`. `avatar/preview.png` shows one frame of every state, and
`avatar/size-options.png` the screen sizes compared. To preview changes without the watch:

```bash
cd ~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk/esp32
python3 tools/muse/make_gifs.py --src "$OLDPWD/avatar/muse_pixel.c" /tmp/avatar_gifs   # needs Pillow
```

The board file sets `.avatar_px = 384` (6× the 64 px grid), which leaves room for the tape label
and the text. A canvas taller than the space above the captions is kept on screen, and the level
meter and captions draw over its lower rows (`muse_ui.c`). `.avatar_dy = 60` then moves it 60 px
down, so the helmet and antennae clear the tape. `avatar/position-options.png` compares the
offsets that were considered.

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

### Screen

**Battery and Wi-Fi ring.** A ring at 2 o'clock with the Wi-Fi symbol inside, above where reply
text starts. It's hidden while the pairing arrow shows, since that points the same way. Charm
hides it for good (`.hide_battery_ring`: a clear face, battery and Wi-Fi in Settings).

- **Charge:** the ring's fill on a dark grey track. White above 30%, yellow from 15 to 30%, red
  below 15%, green while charging. Full on USB is white (the charger keeps topping up at 100%).
- **Wi-Fi symbol:** steady when connected, blinking while joining.
- **Updates:** once a second. The ring is hidden when the watch can't read the battery.
- **Code:** the board places it (`.battery_ring = { LV_ALIGN_CENTER, 156, -90 }`). Setting it also
  hides the SDK's top status line (Wi-Fi, Bluetooth and battery text).

**Dots and ticks instead of status lines.** With `.listen_dots = true`, the voice task's status
lines become marks in the caption's place (`MUSE_CAPTION_DOTS` and `MUSE_CAPTION_SENT` in
`muse_state.h`, drawn in `muse_ui.c`), on the same dark scrim as the captions. They are drawn as
shapes, not set in a font:

- **Listening and sending:** three round white dots that build up, one, two, three, 0.4 s each. The
  level meter and the live transcript are hidden.
- **The mic is on:** while the watch records, the edge ring is a whole circle that follows your
  voice, 30% lit in silence and brighter as you speak (its opacity moves in steps of 16, so a
  steady voice doesn't redraw it), and a small yellow dot shows up and to the left of the battery
  ring. Both go when recording stops. The mic's 320 ms pre-roll buffer, kept in memory while the
  screen is on, doesn't light them: nothing is sent until you talk.
- **Note sent to Muse:** a white tick on a round scrim, for a second; then the dots again until
  Muse's reply arrives.
- **Muse's reply, errors and short messages** ("Speaker on") stay as words.

The level meter, shown on boards without `.listen_dots`, is 14 round dots in the state's colour.

**Replies get the screen.** With `.big_reply = true`, the character stays full size through the
dots and tick, and once Muse's reply text arrives it shrinks to the top centre and the text takes
the rest of the circle: pages of 29 × 6 (up from 13 × 3 while speaking), whether the reply is heard
or only read. The layout picks the widest page that still has 6 lines, centres the text, and the
scrim hugs it. Pages are cut by letter count, so the columns are counted in "o"s rather than M's,
which lets lines run nearly the width of the screen (`avatar/reply-layout-mockup.png`).

**Pairing prompt.** With `.pair_arrow = true`, when the Muse app asks for the talk button, a white
arrow points at the blue button from just inside the rim, with "Press to pair" under it
(`avatar/pair-arrow-mockup.png`). The six-digit passkey still shows on its card.

**No page dots.** `.hide_page_dots = true` drops the two dots at the bottom that mark the main
screen and settings; settings are a swipe left.

**Hidden rim icons.** `.hide_rim_hints = true` hides the mic, speaker and power icons beside the
buttons. The yellow button still briefly shows "Speaker on" or "Speaker off".

**Text.**

- **Font:** replies and captions use Montserrat 20 (`caption_font()`). For characters Montserrat
  lacks, it falls back to the pixel font.
- **State word:** hidden (`.hide_state_label = true`), since the clips show what the watch is doing.
  Without it, "Listening", "Thinking" and so on are in white Montserrat Bold 20
  (`components/muse/fonts/muse_font_bold_20.c`, made with `lv_font_conv`, SIL OFL).
- **Sentence case:** all-caps states and status lines are shown in sentence case ("SPEAKER ON"
  becomes "Speaker on"; `sentence_case()`). Muse, Wi-Fi and USB keep their capitals. Muse's replies
  are left as written.
- **Scrim:** a soft dark band (black, 70% opaque, rounded) sits behind the state word, captions
  and replies, so white text reads clearly over the white helmet.
- **Colours:** black and white. Captions, page dots and the settings pages' switches, sliders and
  buttons are white or grey rather than violet. The edge ring is white on a dark grey track: faint
  while listening and replying, solid while Muse works on a reply (a full circle rather than a
  spinner, with `.listen_dots`). The level meter, where shown, takes each state's colour from the
  astronaut's suit (`ACCENT` in `avatar/character/muse_pixel.c`).

**Tape label.** `.tape_label = true` draws "MOBOT" on worn white painter's tape across the
top-left: grime, smudges, rubbed marker, and the **build number** in blue ballpoint, like
`#28·23·10` (build 28, made 23 Oct). It's one image drawn over everything, like tape on the glass.
The tape, the battery ring (on boards that show it) and the voice's bubble are hidden on the
settings pages.

- **Build number:** `#N` is the number of commits on the branch, with a `+` when `patches/`,
  `avatar/` or `scripts/` have uncommitted changes, so a test build can't pass for a committed one.
  `build.sh` writes it, with the commit and the time, to the SDK's `components/muse/tape/build_id.h`
  and redraws the tape with it for every build. The firmware logs it at boot
  (`MOBOT build #28 (96046bc, built …)`) and the settings page shows it
  (`MOBOT #28 (96046bc) - <IP>`), in place of the SDK's placeholder version 999.0.0.
- **Redrawing needs** [uv](https://docs.astral.sh/uv/) (for Pillow) and Permanent Marker
  (Apache 2.0, Google Fonts), which `build.sh` downloads once to
  `~/Library/Caches/stopwatch-muse-gadget/fonts/` and checks against a pinned SHA-256. Without
  them, the build uses the committed `avatar/tape.rgb565a8` and the number is only in the log and
  settings.
- **Files:** `avatar/make_tape.py` (with `avatar/tape_art.py`) draws the tape; `--note` sets the
  ballpoint text, and `--out`/`--preview` where it goes. `avatar/tape.rgb565a8` and `tape.png` are
  the committed fallback, with the original "A7·22·10·K" note. By hand:
  `python3 avatar/make_tape.py FONTS_DIR --note "#28·23·10"`.

### Talking: live conversations with Grok and Muse

The board file sets `.live_talk = true` (with `.conversation_talk`), so a long press on blue opens
a spoken conversation with xAI's live voice (`wss://api.x.ai/v1/realtime`, `muse_live.c`) rather
than sending voice notes to Muse. It uses the xAI key already stored for speech (`>xai_key=...`);
without one the watch says "SET AN XAI KEY". It connects only for the conversation.

- **Grok talks; Muse does the rest.** Grok (voice `iris`, your Muse persona, never mentioning
  the device; if asked, it's Charm) answers only small talk and simple facts a quick web search
  answers. Everything else goes to Muse (Mobot, your agent in Muse, with its connectors: calendar
  and tasks, email and messages, documents, the home's Hue lights, health, money, shopping,
  travel, music and social, research, family logistics, projects and goals) with its `ask_muse`
  tool, as a typed Muse turn, with your exact words, names, dates and constraints, and never
  passwords, card numbers or codes; Muse asks you before it sends, buys, books or deletes. Muse's
  answer, once the conversation is quiet, plays its faces as reactions and is read out word for
  word in Grok's voice (`force_message`), after Grok says "Let me check." A long answer (research,
  a list) is briefed first: its first sentences, about 200 characters, and "The full answer is in
  your Muse app." Grok never asks whether there's anything else afterwards: when you're done you
  say so, and the conversation ends on its own. Grok shows faces itself with `set_mood` when a
  reply has a clear feeling.
- **Tricks:** ask for something fun ("blow some bubbles", "juggle", "wave", "do a trick") and the
  voice does it itself with `do_trick` (never Muse): the character blows bubbles (6 s), juggles
  planets (8 s) or waves, once through, while the voice says a playful line. Unlike a face, a trick
  isn't cut short when the conversation goes back to listening. The log says `trick: bubbles`.
- **What Grok knows about you:** a short profile (name, city, people, projects, how you like
  answers) and your timezone, kept in NVS. After a conversation, if it's missing or a day old,
  the watch quietly asks Muse for it again (a typed turn you'll see in Muse's history). Grok gets
  it at the start of each conversation, with the local date and time (from xAI's `Date` header:
  the watch has no clock). `>profile?` prints it, `>profile!` asks Muse for it again (the old one
  stays in use until the new one comes), `>profile=...` sets your own (never replaced; first lines
  `TZ: <POSIX TZ>` and `PLACE: City, Region, CC` set the timezone and place), and `>profile=`
  forgets it, place included, until Muse is next asked.
- **Who's answering:** Grok's, Gemini's or Muse's logo (20 px, `avatar/icons`, made by
  `avatar/make_icons.py` and copied in by `build.sh`) in a 60 px soap bubble at 3 o'clock, 20 px
  in from the screen's edge (about a Pixel Watch app's margin; at 24 the astronaut's happy hop
  would touch it), bobbing gently while it's out (6 px, 3.2 s a bob), while that one answers, and Muse's while Muse is working on a question
  (a spinner runs round the bubble's rim while we wait). The bubble blows up in 0.2 s; when
  nobody has been answering for 0.15 s it bursts: the logo goes, the bubble swells and fades,
  and eight droplets fly out, over 0.3 s (`bubble_tick()` in `muse_ui.c`).
- **Turns** end by xAI's voice detection. Every 20 ms the voice task plays a chunk of the reply
  and reads one from the mic; the mic streams to xAI except while the speaker plays and for
  300 ms after. While it listens, only a voice near Charm goes through (`muse_near.c`, from
  Mobot's NearVoice): 6 dB over the room's noise as it's followed (10 dB in a loud room) and at
  least −50 dBFS, with the 300 ms before it and held 600 ms through pauses; people talking across
  the room, which xAI's voice detection would take for a turn, go as silence. The room has a
  floor, so a mic starting on digital silence can't drag it to nothing.
- **On the wire** (following xAI's Voice Agent guide): audio goes both ways as binary WebSocket
  frames, no base64. The mic goes up as 8 kHz G.711 mu-law (11 KB/s): the watch's 2.4 GHz upload
  can fall to about 10 KB/s, under the 43 KB/s base64 16 kHz PCM needs. Replies come down as
  16 kHz PCM, 2 dB quieter than sent (Gemini's, hotter, 6 dB) before the speaker's chain
  (`muse_audio.c`, `CONFIG_MUSE_SPEAKER_BOOST_DB` boards): a 250 Hz low cut (the small speaker
  can't play a voice's lows, which only eat its headroom), +12 dB, and a 3 ms look-ahead limiter
  at −1 dBFS whose gain eases down before a peak and back over 100 ms. The one before it jumped
  its gain to each peak a sample at a time, and loud words crackled. When the network falls
  behind and a reply runs dry, its last chunk eases out over 4 ms and the next eases back in,
  rather than clicking into silence and out again. A reply plays once 200 ms of it is buffered;
  after one has run dry, the next replies in that conversation buffer 200 ms more first, up to
  600 ms. `reasoning.effort` stays at xAI's default (with `none` Grok said its tool calls out
  loud), and transcription gets an English hint and key terms. Up to 4 s of mic is kept while connecting; later, more than 1 s behind is dropped.
  The log shows `mic to xAI: 21.0s so far (0.0s dropped behind)` every 5 s and any send over
  250 ms.
- **Talking over a reply** (barge-in): with no echo canceller on the board, `muse_echo.c` learns
  how loud the speaker comes back in the mic from the first 500 ms of each reply, against what
  actually left the speaker (after its boost and limiter, which cuts loud words up to 8 dB: the
  level before it put the echo 15 dB off), and 200 ms of mic 10 dB over that may be a voice. A mic
  chunk that clips (samples at full scale) while the speaker plays is the speaker overloading it,
  learnt as echo and never a voice; a loud voice that doesn't clip still is. The reply is
  ducked 12 dB and the mic goes to xAI for 1.5 s: if xAI's voice detection hears speech, Grok
  stops (xAI cancels it itself); if not, it was a noise, the reply goes on and that audio is
  discarded. The log says `maybe a barge-in`, then `barge-in: xAI heard you` or
  `not a barge-in: xAI heard no speech`, and after each reply how loud the mic heard the speaker.
- **Live facts:** the weather, news, sports and anything current go to the voice's own web
  search (xAI's `web_search`, Gemini's Google Search), located at your city from the profile's
  `PLACE:` line: about a second, against Muse's five to fifteen.
- **On screen:** only listening or speaking, never a waiting state or dots (the mic is open the
  whole time), and no lit ring (one that followed the voice redrew the whole screen at every
  step and halved the frame rate). While it listens, a yellow arc on the rim at 6 o'clock follows
  your voice: 14° in silence, growing to 90° for a loud voice (LVGL redraws only its moving ends).
  Its level is the voice over the room's noise, averaged over 100 ms rather than each 20 ms chunk
  (which spiked on every syllable), and it moves like a critically damped spring, settling in
  0.25 s growing and 0.5 s falling, so it never jerks and the room leaves it still (as Mobot's arc
  does). It's on the face, so it slides off with it to settings; the bubble just goes.
  Captions show the reply a page at a time: three lines of about 22 letters (that low, the round
  screen is narrow), the whole page being spoken shown at once and kept until the speech reaches
  the next, a page ending at a sentence's end where one falls at least a quarter of the way in,
  rather than the last lines moving on with every word. The last words stay 2.5 s after the reply
  (or after a barge-in cuts it), then go. They're left-aligned a little above the arc on their
  scrim over the full-size character. The voice's logo is in its bubble at 3 o'clock.
- **A click** while it speaks stops the reply and the conversation goes on; while it listens,
  a click ends the conversation. The top-left button (the speaker) says "SPEAKER ON/OFF"
  mid-conversation too: with it off you hear nothing.
- **It ends** on a click; when you say goodbye or you're clearly done (the voice says goodbye and
  calls `end_conversation`; it closes once that's heard, or 6 s after the call if no goodbye
  comes); with nobody talking, nothing playing and nothing waiting on the voice or Muse for 5 s
  after the press (7 s once there's been a reply: time to think); or if the connection fails,
  whose reason stays up ("CAN'T REACH XAI", "LOST GEMINI"...). Someone talking by the mic (12 dB
  over the room it learns) keeps it open too, the voice's word that it heard you coming late, but
  for no more than 15 s past the voice's last sign. Muse's answer gets up to 20 s to start being
  read, after which it's given up (it would hold the conversation open); talking over it, or a
  click, drops it at once, and on xAI a click before a reply has started cancels it there too. The screen doesn't sleep during a conversation, and its 30 s sleep starts from the end.
- **Connecting:** a first TLS handshake with xAI or Google takes 5-8 s on the watch, a resumed
  one a few hundred ms, so the watch does one in the background once Wi-Fi is up, on waking and
  on a talk press (`muse_live_warm()`; no session, nothing billed).
- **The profile** is asked of Muse two quiet minutes after a conversation (put off by any that
  starts meanwhile): Muse takes one request at a time, and a question asked while the profile
  was coming waited behind it.
- **Two live voices:** xAI's Grok (the default) or Google's Gemini Live
  (`gemini-3.1-flash-live-preview`, voice Leda), chosen in Settings > Voice or over serial with
  `>live_provider=gemini` or `>live_provider=xai` (from the next conversation; `>live_provider?`
  says which) and kept in NVS. Gemini uses the stored Gemini
  key (`>gemini_key=...`). It costs about a sixth as much a minute (Google bills audio streamed,
  about $0.005/min in and $0.018/min out; xAI bills $0.08 for every minute a session is open).
  The differences:
  - Audio goes up as base64 8 kHz PCM in JSON (Gemini has no binary or mu-law input) and comes
    down at 24 kHz, resampled to 16 kHz on the watch (`muse_resample.c`).
  - There's no verbatim speech, so Muse's answer goes back as the tool's result with a note to
    say it word for word.
  - It searches with Google Search, and its logo is Gemini's star.
  - It can't be interrupted by voice. With the speaker coming back 10 dB over a voice in this
    mic, it heard and believed its own words. A click stops its reply instead (with either
    voice: a click while it listens still ends the conversation).
  - It followed the routing less well, so for it the instructions call it Muse itself and the
    tool `assistant_request`; and the watch answers a call that's only small talk ("hi, how are
    you?") itself (`small_talk()`: a greeting cue and nothing of substance left), sparing Muse.
  - Its tool calls block, so it can't say "Let me check" first (it would come after the answer);
    it says nothing, and the logo's spinner shows the wait. It sometimes ends its turn on Muse's
    answer without saying it: then the watch nudges it once with a short text turn (or after 8 s
    of nothing at all from it: a nudge while it's still working on a long answer would cut it off).
- **Testing it end to end:** `uv run --no-project --with pyserial python scripts/e2e_live.py
  [PORT]`, with Charm idle, drives a conversation over serial: presses talk, speaks a question
  (`say` audio fed where the mic goes, `>mic=<base64 PCM>`) and checks the log for Muse's answer
  and its read-out and a spoken goodbye. That's the quick check (about 40 s on the current voice;
  `--provider` picks the other): every conversation is a paid live session, so it's the default.
  `--all` runs everything on both voices (about 13 min, for a commit): typed questions
  (`>live_ask=...`) too, small talk, the weather, Muse questions and their read-out, a home
  question, a trick (bubbles, not sent to Muse), a click over a reply, a goodbye, silence, and
  Muse straight after a reboot, plus no stuck loop, no errors and internal RAM never running out.
- The protocol and instructions are the live-voice prototype's (`prototype/live-voice/orchestrator.js`,
  branch `live-voice-prototype`), which the simulator keeps matching. `tests/test_muse_echo.py`
  feeds the echo gate a reply's echo, a voice over it and the StopWatch's loud speaker;
  `tests/test_muse_near.py` feeds the near-voice gate a room, talk across it, a voice at arm's
  length and a mic starting on silence.

### Talking: hands-free voice notes (without live_talk)

With `.conversation_talk` and no `.live_talk`, the blue button starts a hands-free conversation of
voice notes to Muse (`converse()` in `muse_voice.c`):

```
 idle ──long press (0.5 s)──► listening ──you speak, then 0.9 s quiet──► thinking ──► speaking
  ▲                               ▲                                                     │
  │                               └───────────────────── the reply ends ───────────────┘
  └── a click (listening or speaking), or nobody speaks: 5 s after the press, 4 s after a reply
```

- **Each turn ends when you go quiet,** whether or not you're still holding the button.
  `muse_vad.c` learns the room's level from the 320 ms pre-roll and keeps following it (down at
  once, up at 5 dB a second, and across turns): speech is 60 ms at 12 dB over it (and at least
  −48 dBFS), and 0.9 s at under 6 dB over it ends the turn. The log says why each turn ended:
  `hands-free turn: quiet after speech after 3.2s, room -55 dBFS`.
- **The next turn starts after the reply,** skipping 200 ms of the speaker's own tail. The ring
  follows your voice and the yellow dot shows while it listens.
- **A reply can't hang:** if speech stops arriving for 10 s while xAI still says it's speaking
  (`REPLY_STALL_US`), the watch ends the reply and the conversation and goes idle, logging
  `reply stalled`. While it speaks it logs `reply: Ns played` every 3 s, and xAI's worker logs
  `reply buffer full` if its 16 s buffer stays full for 2 s.
- **It ends** when a turn hears nobody (5 s after the long press, 7 s after a reply: nothing is
  sent), on a click of blue while it listens or speaks (a click stops the reply too), or when a
  note fails or is saved for later, whose caption stays up. A click while idle only says
  "Hold to talk": on boards with the pairing arrow (`.pair_arrow`), the arrow points at the
  button with those words for 3 s; elsewhere, as a caption.
- `LONG_PRESS_MS`, `FIRST_QUIET_MS` and `CONVERSATION_QUIET_MS` in `muse_voice.c`, and the
  `MUSE_VAD_*` levels in `muse_vad.h`, are the numbers to tune. `tests/test_muse_vad.py` feeds the
  detector speech, a loud fan, a louder room and a single click.
- The live-voice web prototype (`prototype/live-voice`, branch `live-voice-prototype`) has the same
  flow, with xAI's voice detection in place of `muse_vad.c`.

### Wake

The board file sets `.power_wakes_only = true`. While asleep, touch and the yellow button are
ignored. Red wakes the watch. Holding blue for 0.3 s wakes it and starts a conversation, and a
shorter click is ignored (`HOLD_WAKE_MS` in `muse_input.c`). A pairing prompt or an image from Muse still wakes the
screen.

## Repo layout

```
stopwatch-muse-gadget/
├── README.md             # this file
├── .gitignore            # build output, SDK checkout, sdkconfig, backups, secrets
├── scripts/
│   ├── install-idf.sh    # ESP-IDF v6.0.1 install (idempotent)
│   ├── build.sh          # clone pinned SDK, apply patches + avatar, build stopwatch firmware
│   └── flash.sh          # factory backup + flash + avatar clips
├── patches/
│   └── 0001-stopwatch-customizations.patch   # speech, screen, battery, wake
├── avatar/
│   ├── character/        # the default avatar: the astronaut as video clips
│   │   ├── muse_pixel.c  # the clip player (stands in for the pixel renderer)
│   │   ├── make_anims.py # packs clips/*.mp4 into anims.pack
│   │   ├── anims.pack    # what flash.sh writes to the "anims" partition
│   │   ├── clips/        # one .mp4 per state
│   │   ├── astronaut.jpg # the character's reference image
│   │   └── preview.gif   # every clip
│   ├── muse_pixel.c      # the pixel-art astronaut (AVATAR=pixel)
│   ├── preview.png       # every state, enlarged
│   ├── battery-preview.png  # the battery ring at 85%, 25%, 8% and charging
│   ├── size-options.png  # on-screen size options compared
│   ├── make_tape.py      # generates the MOBOT tape label (with tape_art.py)
│   ├── tape.rgb565a8     # the tape label the firmware embeds
│   ├── tape.png          # ...and a preview of it
│   ├── status-mockup.png # the battery ring's states, mocked up
│   ├── position-options.png # the character 0–60 px lower under the tape
│   ├── quality-options.png  # clip sizes and JPEG qualities compared
│   ├── pair-arrow-mockup.png # the pairing prompt's arrow at the blue button
│   ├── reply-layout-mockup.png # a reply page with the character small at the top
│   ├── requirements.txt  # Python packages for the avatar tools (Pillow)
│   └── *-options.png, *-sample.png  # mockups from choosing fonts and the tape
├── docs/
│   ├── plan.md           # project plan and status
│   ├── internal-ram.md   # where the StopWatch's internal RAM goes, and the fixes that won it back
│   ├── infra.md          # build, flash and test infrastructure to improve, ranked
│   └── token.md          # SDK token, Gemini and xAI keys: how to set them, never-commit rules
└── backups/              # factory flash backup (gitignored)
```

The SDK checkout isn't in the repo: `build.sh` clones it into
`~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk`, pinned at
`fa073045a5572a8e3367fae142c1b7a7c554f222` and patched. `SDK_DIR=$PWD/.sdk/muse-gadget-sdk` still
puts it in the repo (gitignored), but inside `~/Documents` iCloud gets in the way.

### Changing the SDK patch

Edit the files in the SDK checkout (`~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk`),
build and test, then save the patch:

```bash
./scripts/save-patch.sh
```

It includes every new file under `esp32/components/muse/` (except `tape/`, which `build.sh` copies
in) and `esp32/partitions_stopwatch.csv`, so a new file needs no list. It checks that the patch
applies to a clean checkout of the pinned commit, and scans it for keys: the shapes of a Google or
xAI key or an SDK token, and the copies saved in `~/.config/stopwatch/` (`docs/token.md`). It
replaces `patches/0001-stopwatch-customizations.patch` only if every check passes.

A new file elsewhere in the SDK still needs `git -C <SDK> add -N <file>` before the script runs,
and `git -C <SDK> reset -q <file>` after. Read the diff before committing (`docs/token.md`,
"Never-commit checklist").

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
  or delete `~/Library/Caches/stopwatch-muse-gadget/muse-gadget-sdk` and rebuild.
- **`... is in use`:** another build or flash holds the SDK checkout. The message names it. Wait,
  or set `LOCK_WAIT`. If that process is gone, the next run takes the lock over.
- **Serial port permissions on Linux:** `pip install esptool` gives the serial port the permissions
  it needs on some setups.
