# SDK token and Gemini API key

This firmware uses two secrets. Neither belongs in git.

| Secret | What it's for | Needed to build? | How it gets onto the watch |
| --- | --- | --- | --- |
| Muse Gadget SDK token | Pairing and working with the Muse app | No | `>sdk_token=...` over serial, or menuconfig |
| Gemini API key | Spoken replies (Gemini TTS) | No | `>gemini_key=...` over serial only |

Most of this page covers the SDK token. The Gemini key has
[its own section](#gemini-api-key-spoken-replies) below. The never-commit checklist and the leak
steps apply to both.

The firmware needs a Muse Gadget SDK token to work with the Muse app.

**The build does not need it.** Building without a token only prints a warning. You can build,
flash, and add the token afterwards.

> **Rule: the token never goes into git.** That covers tracked files, commit messages, branch
> names, issues, PR descriptions, and any pasted logs. No exceptions, even though this repo is
> private.

## Getting a token

Generate one at [gadgets.muse.ai/settings/sdk-tokens](https://gadgets.muse.ai/settings/sdk-tokens),
signed into your own Muse account. Accept the Gadget SDK Terms if asked.

## Path A: set it over serial (recommended)

The token is stored on the device. Nothing lands in the build tree.

1. Flash the firmware (`scripts/flash.sh`), with or without a token built in.
2. Open a serial monitor from the SDK's `esp32/` directory:

```bash
   . "$HOME/esp/esp-idf-v6.0.1/export.sh"
   cd .sdk/muse-gadget-sdk/esp32
   idf.py -B build-muse-m5stack-stopwatch -p PORT monitor
```

3. Type this line and press Enter:

```
   >sdk_token=YOUR_TOKEN
```

4. The device answers `@sdk_token ok` and restarts.
5. Exit the monitor with `Ctrl+]`.

**If there's no `@sdk_token ok` reply,** line endings are a likely cause. Try a terminal that sends
LF, for example:

```bash
python -m serial.tools.miniterm --eol LF PORT 115200
```

pyserial ships in the ESP-IDF Python env. Exit miniterm with `Ctrl+]`.

**Keep the token out of your shell history.** Type it into the monitor. Don't run something like
`echo ">sdk_token=..." > /dev/cu...`, because that saves the token in your history.

## Path B: build it into the firmware with menuconfig

The token is compiled into the firmware via Kconfig `CONFIG_GADGET_SDK_TOKEN`.

1. Build once with `scripts/build.sh` so the build directory is configured.
2. Open menuconfig against the same build directory:

```bash
   . "$HOME/esp/esp-idf-v6.0.1/export.sh"
   cd .sdk/muse-gadget-sdk/esp32
   idf.py -B build-muse-m5stack-stopwatch menuconfig
```

3. Go to **ESP32 Device SDK → Muse Gadgets SDK token**, enter the token, save, and exit.
4. Rebuild and reflash:

```bash
   cd -   # back to the repo root
   ./scripts/build.sh
   ./scripts/flash.sh PORT
```

**Where the token ends up with this path:** the generated `sdkconfig`, generated config headers
and JSON in the build directory, and the firmware `.bin` files. This repo gitignores all of these
(`.sdk/`, `build*/`, `sdkconfig*`, `*.bin`). Even so:

- Don't share build directories or firmware binaries.
- Don't paste `sdkconfig` contents anywhere.
- Don't commit an `sdkconfig` "to save the setting." If configuration ever needs to be tracked, use
  a defaults fragment **without** the token.

## Precedence

| Built-in token (menuconfig) | Stored token (serial) | Token the device uses |
| --- | --- | --- |
| none | none | none (build warned; pairing/SDK features won't work) |
| set | none | built-in token |
| none or set | set | **stored token** |

A token stored over serial always wins over the one in the firmware. Consequences:

- **Changing the token:** send a new `>sdk_token=...` line. Rebuilding with a different built-in
  token has no effect while a stored token exists.
- **Reflashing the app** (`scripts/flash.sh`) normally keeps the stored token, because it doesn't
  rewrite the data partition where settings live.
- **Going back to the built-in token, or removing a stored one:** we haven't verified a dedicated
  clear command at the pinned SDK commit. The blunt option is to erase the whole flash and reflash.
  This erases everything, including Wi-Fi and pairing state:

```bash
  . "$HOME/esp/esp-idf-v6.0.1/export.sh"
  esptool --chip esp32s3 -p PORT erase-flash
  ./scripts/flash.sh PORT
```

- **Flash dumps after the token is set contain it.** The factory backup taken by `flash.sh`
  predates any token, but any later `read-flash` dump will include it. Treat those dumps like the
  token itself.

## Gemini API key (spoken replies)

The firmware in this repo speaks Muse's replies through Gemini TTS
(`patches/0001-stopwatch-customizations.patch`, `muse_gemini_tts.c`). Without a key, replies stay
text captions.

### Getting a key

Create one at [aistudio.google.com/apikey](https://aistudio.google.com/apikey), signed into your
own Google account. Put the project on a spending limit, and use a key you can revoke. It sits on
the watch unencrypted, since NVS encryption is deferred, so anyone holding the watch could read it.

### Setting it

There's no menuconfig path: the key is only ever set over serial and stored in the watch's NVS
(namespace `muse_tts`, key `gemini_key`). It never enters the build tree or a firmware image, and
it survives reflashing.

1. Open a serial monitor, as in Path A above, or run
   `python -m serial.tools.miniterm --eol LF PORT 115200`.
2. Type this line and press Enter:

   ```
   >gemini_key=YOUR_KEY
   ```

3. The watch answers `@gemini_key ok`. No restart is needed. The next reply is spoken.

Other answers:

- **`@gemini_key error invalid`:** the key is shorter than 20 characters, or has characters other
  than letters, digits, `-`, `_` and `.`.
- **`@gemini_key error storage`:** NVS couldn't save it. Try again, and check the serial log.

To remove it, send `>gemini_key=` with nothing after it. The watch answers `@gemini_key cleared`,
and replies go back to captions.

The firmware never logs the key. Its log only says `Gemini key stored` or `Gemini key cleared`.

### Keeping a copy on your Mac

To test the API from the Mac, or to resend the key after erasing the flash, keep a copy outside the
repo. This prompt doesn't echo what you type, and nothing lands in shell history:

```bash
mkdir -p ~/.config/stopwatch && read -rs "?Gemini API key: " k && printf %s "$k" > ~/.config/stopwatch/gemini_key && chmod 600 ~/.config/stopwatch/gemini_key && unset k
```

Scripts read it with `$(cat ~/.config/stopwatch/gemini_key)`. Don't `echo` it or paste it into
chats, issues or logs.

## Never-commit checklist

Before every commit:

```bash
git status --short          # nothing under .sdk/, build*/, sdkconfig*, *.bin, backups/
git diff --cached           # read what you're actually committing
```

To scan tracked content for a token, search for a distinctive fragment of your real token. Paste
it at the prompt rather than putting it on the command line, so it doesn't land in shell history:

```bash
read -rs FRAG && git grep -n -F -- "$FRAG"; unset FRAG
```

No output means the fragment isn't in any tracked file.

For the Gemini key, check the staged changes and the history against the saved copy, plus the
shape of any Google key:

```bash
git diff --cached | grep -cF -- "$(cat ~/.config/stopwatch/gemini_key)"    # must print 0
git log -p --all | grep -cF -- "$(cat ~/.config/stopwatch/gemini_key)"    # must print 0
git grep -nE 'AIza[0-9A-Za-z_-]{30,}|mgst_[0-9A-Za-z]{8,}'               # must print nothing
```

When you regenerate the patch, check it too. It's generated from the SDK checkout, so anything
pasted into an SDK file would end up in it.

## If the token or key leaks

If the token or the Gemini key ever reaches git (even an unpushed local commit), a public place, or
a shared log:

1. Treat it as compromised. Revoke or regenerate it wherever you obtained it: gadgets.muse.ai for
   the token, or AI Studio / Google Cloud console for the Gemini key.
2. Remove it from history. Deleting it in a new commit is not enough, since the old commit still
   contains it. Rewrite history (for example with `git filter-repo`) and force-push.
3. Set the new token on the device over serial (Path A), or the new key with `>gemini_key=...`.
