# stopwatch-muse-gadget

Turn an **M5Stack StopWatch** dev kit (ESP32-S3) into a **Muse Gadget** using Meta's official [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk).

Pinned SDK commit: `fa073045a5572a8e3367fae142c1b7a7c554f222`
Toolchain: ESP-IDF v6.0.1 (exact)

> **Hardware status:** StopWatch dev kit order **M5-113746** is in transit. The board (ESP32-S3) is officially supported by the SDK: `esp32/devices/sdkconfig.muse-m5stack-stopwatch` plus `esp32/components/muse/boards/board_m5stack_stopwatch.c`.

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

This clones the SDK at the pinned commit into `.sdk/muse-gadget-sdk` (override with `SDK_DIR`), finds ESP-IDF v6.0.1's `export.sh` automatically, and runs `tools/muse/board.sh build stopwatch` from the SDK's `esp32/` directory. `tools/muse/board.sh build stopwatch` sets the IDF target to `esp32s3` and `SDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch"`; the build dir is `build-muse-m5stack-stopwatch`.

No SDK token needed to build. The build only warns about the missing token; that's expected. (See
`docs/token.md`.)

### 3. Flash

```bash
./scripts/flash.sh            # auto-detects the serial port
./scripts/flash.sh /dev/cu.usbmodem101   # or pass it explicitly
```

**On the first flash, the script backs up the 16 MB factory firmware to
`backups/stopwatch-factory-<date>.bin` (`esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 ...`) and writes a
`.sha256` checksum next to it before flashing. Later runs detect the existing backup and skip
straight to flashing — you keep one golden copy of the factory image. Then it flashes with
`tools/muse/board.sh flash stopwatch [PORT]`.

Verify the backup before flashing away the original:

```sh
shasum -a 256 -c backups/stopwatch-factory-*.bin.sha256   # macOS
sha256sum -c backups/stopwatch-factory-*.bin.sha256       # Linux
```

### 4. Add the SDK token

Pick **one** of these:

- **Recommended:** send `>sdk_token=...` over serial after flashing — the device replies
  `@sdk_token ok` and restarts.
- Or bake it in with `idf.py menuconfig` (**ESP32 Device SDK** → **Muse Gadgets SDK token**), then rebuild and reflash.

Full details, precedence rules, and the never-commit policy: `docs/token.md`.

### 5. Pair

1. In the Muse app: **Settings → Devices → Developer mode**.
2. Join your trusted home Wi-Fi on both the phone and the device when prompted.
3. Hold the blue top-right button to talk.

## Buttons

| Button | Position | Function |
| --- | --- | --- |
| Blue | top-right | Talk |
| Yellow | top-left | Speaker |
| Red | bottom-left | Power |

## Repo layout

```
stopwatch-muse-gadget/
├── README.md             # this file
├── .gitignore            # build output, SDK checkout, sdkconfig
├── scripts/
│   ├── install-idf.sh    # ESP-IDF v6.0.1 install (idempotent)
│   ├── build.sh          # clone pinned SDK, build stopwatch firmware
│   └── flash.sh          # factory backup + flash
├── docs/
│   ├── plan.md           # design notes (added separately)
│   └── token.md          # SDK token: two paths, precedence, never-commit rule
└── .sdk/                 # SDK checkout (gitignored; created by build.sh)
    └── muse-gadget-sdk/  # pinned at fa073045a5572a8e3367fae142c1b7a7c554f222
```

## Safety notes

- **Back up the factory firmware before the first flash.** `scripts/flash.sh` does this
  automatically (`backups/stopwatch-factory-<date>.bin` + `.sha256`), but verify the checksum and copy
  the backup somewhere safe (not just the repo, and definitely not an unencrypted external drive).
  `*.bin` files are gitignored on purpose.
- **The SDK token never goes into git.** Not in files, commit messages, or pasted logs — even
  though this repo is private. See `docs/token.md`.
- **Pair only on trusted Wi-Fi.** Your home network, not café or hotel networks. The Muse app pairs
  the gadget in developer mode over the same network.
- **NVS encryption is deferred.** Enabling it can burn ESP32-S3 eFuses, which is irreversible, so
  first-light runs without it. Revisit before any long-term deployment.

## Troubleshooting

- `idf.py` not found → run `./scripts/install-idf.sh`, or set `IDF_EXPORT` to your v6.0.1
  `export.sh`.
- No serial port → plug the StopWatch in over USB-C and rerun `./scripts/flash.sh`; on macOS it shows
  up as `/dev/cu.usbmodem*` (note the `cu.` prefix).
- Token warning at build time is expected — the build succeeds without a token.
- `pip install esptool` gives the serial port the permissions it needs on some Linux setups.
