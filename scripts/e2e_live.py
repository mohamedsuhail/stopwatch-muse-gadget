#!/usr/bin/env python3
"""End-to-end test of live conversations on a flashed Charm, over its serial port.

    uv run --no-project --with pyserial python scripts/e2e_live.py [PORT]            # the quick check
    uv run --no-project --with pyserial python scripts/e2e_live.py [PORT] --all      # everything, before a commit
    ... --provider gemini|xai|both   (quick: the one; default the current one. --all: default both)

Every conversation is a paid live session (xAI bills each minute one is open), and Muse is
really asked, so the default is the quick check: one conversation on the current voice, with a
spoken question for Muse, its answer read out, and a spoken goodbye (about 40 s). --all runs
every scenario on the voices chosen, the hand-off (Muse made slow: the voice says Muse is on it and
the answer is read out when it comes) and the cold start after a reboot (about 14 min for both);
--handoff runs just the hand-off.

It drives Charm the way a person would, except that questions are typed
(">live_ask=...", which the live voice gets as if it had heard them) rather
than spoken, and spoken questions are fed in place of the mic (">mic=...",
16 kHz PCM from macOS's `say`, in real time), so the voice's own turn detection
and transcription are tested too: it presses talk over serial to start a conversation, asks small
talk, a trick, the weather, a personal question for Muse, clicks over a reply, says
goodbye, and stays quiet, and checks the watch's log for each step. Then it
reboots the watch and asks Muse straight away (the cold first request). It
needs the watch idle (nobody using it), an xAI and/or Gemini key on it, and
Muse set up. It leaves the provider as it found it (">live_provider?").
"""
import argparse, atexit, base64, os, re, subprocess, sys, tempfile, threading, time, wave

import serial

ap = argparse.ArgumentParser()
ap.add_argument("port", nargs="?", default="/dev/cu.usbmodem101")
ap.add_argument("--provider", default=None, choices=["gemini", "xai", "both"])
ap.add_argument("--all", action="store_true", help="every scenario, not the quick check")
ap.add_argument("--handoff", action="store_true", help="just the hand-off: a slow Muse, the answer read out later")
args = ap.parse_args()

ser = serial.Serial(args.port, 115200, timeout=0.2)
log = bytearray()
lock = threading.Lock()
running = True


def reader():
    while running:
        data = ser.read(4096)
        if data:
            with lock:
                log.extend(data)


threading.Thread(target=reader, daemon=True).start()

# macOS's `say` takes a few seconds to load a voice the first time: warmed now, so the first
# spoken question doesn't come after Charm has given up waiting (5 s after the press).
with tempfile.TemporaryDirectory() as _tmp:
    subprocess.run(["say", "-r", "175", "-o", os.path.join(_tmp, "warm.aiff"), "Ready."], check=True)


def text_from(mark):
    with lock:
        return log[mark:].decode(errors="replace")


def mark():
    with lock:
        return len(log)


def wait_for(pattern, since, timeout):
    """The first line matching `pattern` after `since`, or None after `timeout` s."""
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(pattern, text_from(since))
        if m:
            return m
        time.sleep(0.2)
    return None


def send(data):
    ser.write(data)
    ser.flush()


def press(hold):
    send(b"d")
    time.sleep(hold)
    send(b"u")


results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{'  - ' + detail if detail else ''}", flush=True)


def start_conversation():
    send(b"w")   # awake
    time.sleep(1)
    m0 = mark()
    press(0.9)   # a long press
    ok = wait_for(r"session set up in (\d+) ms", m0, 25)
    return m0, ok


def ask(text, since=None):
    m0 = mark() if since is None else since
    send(f">live_ask={text}\n".encode())
    return m0


def speak(text):
    """Says `text` into the conversation: `say`, at 16 kHz, streamed as the mic would be, then
    1.5 s of silence so the voice hears the turn end. Returns the log mark from its start."""
    with tempfile.TemporaryDirectory() as tmp:
        aiff, wav = os.path.join(tmp, "q.aiff"), os.path.join(tmp, "q.wav")
        subprocess.run(["say", "-r", "175", "-o", aiff, text], check=True)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff, wav], check=True)
        w = wave.open(wav)
        pcm = w.readframes(w.getnframes()) + b"\0" * (16000 * 2 * 3 // 2)
    m0 = mark()
    t0 = time.time()
    print(f"        (speaking {len(pcm) / 32000:.1f} s of audio)", flush=True)
    for i, off in enumerate(range(0, len(pcm), 640)):
        send(b">mic=" + base64.b64encode(pcm[off:off + 640]) + b"\n")
        delay = t0 + (i + 1) * 0.02 - time.time()
        if delay > 0:
            time.sleep(delay)
    return m0


def conversation_over(since, timeout):
    return wait_for(r"live conversation: ended \((\w+)\)", since, timeout)


def run(provider):
    print(f"\n== {provider} ==", flush=True)
    send(f">live_provider={provider}\n".encode())
    time.sleep(1)

    # 1. Small talk: answered by the voice itself, not Muse.
    m0, ok = start_conversation()
    check(f"{provider}: conversation starts", bool(ok), ok and f"set up in {ok.group(1)} ms")
    if not ok:
        return
    m = ask("Hi there! How are you doing today?")
    played = wait_for(r"reply played", m, 25)
    check(f"{provider}: small talk answered", bool(played))
    check(f"{provider}: small talk not sent to Muse", "asking Muse" not in text_from(m))

    # 1b. A trick: the voice plays it itself, never asking Muse.
    m = ask("Can you blow some bubbles for me?")
    trick = wait_for(r"trick: (\w+)", m, 25)
    check(f"{provider}: asked for bubbles, it does a trick", bool(trick), trick and trick.group(1))
    wait_for(r"reply played", m, 25)
    check(f"{provider}: the trick not sent to Muse", "asking Muse" not in text_from(m))

    # 2. The weather: web search, not Muse.
    m = ask("What's the weather like right now?")
    played = wait_for(r"reply played", m, 35)
    check(f"{provider}: weather answered", bool(played))
    check(f"{provider}: weather not sent to Muse", "asking Muse" not in text_from(m))

    # 3. Personal: Muse is asked, answers, and the answer is read out.
    m = speak("What's on my calendar today?")   # spoken: typed questions that need Muse, Gemini answers late
    asked = wait_for(r"asking Muse", m, 20)
    check(f"{provider}: calendar goes to Muse", bool(asked))
    answered = wait_for(r"Muse answered in ([\d.]+)s", m, 90)
    check(f"{provider}: Muse answers", bool(answered), answered and f"{answered.group(1)} s")
    reading = wait_for(r"reading Muse's answer", m, 30)
    check(f"{provider}: answer handed to the voice", bool(reading))
    if reading:
        after = reading.end()
        played = None
        end = time.time() + 45
        while time.time() < end and not played:
            played = re.search(r"reply played", text_from(m)[after:])
            time.sleep(0.3)
        check(f"{provider}: Muse's answer read out", bool(played))
    check(f"{provider}: conversation still open after Muse", "live conversation: ended" not in text_from(m))

    # 3b. The home (Muse's connectors), read-only so nothing in the house changes.
    m = speak("Are the kitchen lights on right now?")
    asked = wait_for(r"asking Muse", m, 20)
    check(f"{provider}: the kitchen lights go to Muse", bool(asked))
    if asked:
        wait_for(r"Muse answered|couldn't answer", m, 90)
        wait_for(r"reply played", m, 45)

    # 3c. About you, in the first person: Muse's, never small talk (read-only).
    m = ask("How am I doing today?")
    asked = wait_for(r"asking Muse", m, 20)
    check(f"{provider}: \"how am I doing\" goes to Muse", bool(asked))
    if asked:
        wait_for(r"Muse answered|couldn't answer", m, 90)
        wait_for(r"reply played", m, 45)

    # 4. A click over a reply stops it, and the conversation goes on.
    m = ask("Tell me a story about a friendly dragon, in five long sentences.")
    time.sleep(7)   # it's speaking by now
    press(0.15)
    stopped = wait_for(r"reply stopped \(click\)", m, 5)
    check(f"{provider}: click stops the reply", bool(stopped))
    m2 = ask("Thanks. What is two plus two?")
    played = wait_for(r"reply played", m2, 25)
    check(f"{provider}: conversation goes on after the click", bool(played))

    # 5. Goodbye closes it, after the goodbye is heard.
    m = speak("Thanks, that's all for now. Goodbye!")
    over = conversation_over(m, 40)
    check(f"{provider}: goodbye ends it", bool(over) and over.group(1) == "goodbye",
          over and f"ended ({over.group(1)})")
    if over:
        said = re.search(r"reply played", text_from(m)[: over.start()])
        check(f"{provider}: the goodbye is heard first", bool(said))
    time.sleep(3)

    # 6. Spoken, through the voice's own turn detection and transcription.
    m0, ok = start_conversation()
    if ok:
        m = speak("What's on my calendar today?")
        heard = wait_for(r'you: "([^"]*)"', m, 30)
        check(f"{provider}: spoken question heard", bool(heard) and "calendar" in heard.group(1).lower(),
              heard and f'"{heard.group(1)}"')
        answered = wait_for(r"Muse answered in ([\d.]+)s", m, 90)
        check(f"{provider}: spoken question answered by Muse", bool(answered), answered and f"{answered.group(1)} s")
        reading = wait_for(r"reading Muse's answer", m, 30)
        played = None
        if reading:
            end = time.time() + 45
            while time.time() < end and not played:
                played = re.search(r"reply played", text_from(m)[reading.end():])
                time.sleep(0.3)
        check(f"{provider}: spoken question's answer read out", bool(played))
        m = speak("Hey, how are you doing?")
        played = wait_for(r"reply played", m, 30)
        check(f"{provider}: spoken small talk answered", bool(played))
        check(f"{provider}: spoken small talk not sent to Muse", "asking Muse" not in text_from(m))
        m = speak("Thanks, that's all. Goodbye!")
        over = conversation_over(m, 40)
        check(f"{provider}: spoken goodbye ends it", bool(over) and over.group(1) == "goodbye",
              over and f"ended ({over.group(1)})")
        time.sleep(3)

    # 7. Silence closes it.
    m0, ok = start_conversation()
    over = conversation_over(m0, 30)
    check(f"{provider}: silence ends it", bool(over) and over.group(1) == "quiet", over and f"ended ({over.group(1)})")
    time.sleep(3)


def cold_start(provider):
    print(f"\n== {provider}: Muse straight after a reboot ==", flush=True)
    m0 = mark()
    ser.dtr = False
    ser.rts = True
    time.sleep(0.2)
    ser.rts = False
    wait_for(r"setup=done wifi=up ws=up", m0, 40)
    time.sleep(3)
    m0, ok = start_conversation()
    check(f"{provider} cold: conversation starts", bool(ok), ok and f"set up in {ok.group(1)} ms")
    if not ok:
        return
    m = ask("What did I work on today?")
    answered = wait_for(r"Muse answered in ([\d.]+)s", m, 90)
    check(f"{provider} cold: Muse answers", bool(answered), answered and f"{answered.group(1)} s")
    played = wait_for(r"Muse's answer\)?: |reply played", m, 60) if answered else None
    check(f"{provider} cold: answer read out", bool(played))
    press(0.15)   # end it
    time.sleep(3)


def save_log():
    with lock:
        data = bytes(log)
    with open("/tmp/e2e_live.log", "wb") as f:
        f.write(data)


atexit.register(save_log)   # kept even if the run dies (the port gone, a crash)

def quick(provider):
    """One conversation: a spoken question for Muse, its answer read out, a spoken goodbye."""
    print(f"\n== {provider}: quick check ==", flush=True)
    send(f">live_provider={provider}\n".encode())
    time.sleep(1)
    m0, ok = start_conversation()
    check(f"{provider}: conversation starts", bool(ok), ok and f"set up in {ok.group(1)} ms")
    if not ok:
        return
    m = speak("What's on my calendar today?")
    heard = wait_for(r'you: "([^"]*)"', m, 30)
    check(f"{provider}: spoken question heard", bool(heard) and "calendar" in heard.group(1).lower(),
          heard and f'"{heard.group(1)}"')
    answered = wait_for(r"Muse answered in ([\d.]+)s", m, 90)
    check(f"{provider}: Muse answers", bool(answered), answered and f"{answered.group(1)} s")
    reading = wait_for(r"reading Muse's answer", m, 30)
    played = None
    if reading:
        end = time.time() + 45
        while time.time() < end and not played:
            played = re.search(r"reply played", text_from(m)[reading.end():])
            time.sleep(0.3)
    check(f"{provider}: Muse's answer read out", bool(played))
    m = speak("Thanks, that's all. Goodbye!")
    over = conversation_over(m, 40)
    check(f"{provider}: spoken goodbye ends it", bool(over) and over.group(1) == "goodbye",
          over and f"ended ({over.group(1)})")
    time.sleep(3)


def handoff(provider):
    """Muse made slow (handed off after 0.5 s): the voice says Muse is on it, the conversation ends on a
    goodbye, and the answer is read out when it comes, in the conversation or after it."""
    print(f"\n== {provider}: hand-off, Muse slow ==", flush=True)
    send(f">live_provider={provider}\n".encode())
    time.sleep(1)
    send(b">live_handoff_ms=500\n")
    time.sleep(0.5)
    try:
        m0, ok = start_conversation()
        check(f"{provider}: conversation starts", bool(ok), ok and f"set up in {ok.group(1)} ms")
        if not ok:
            return
        m = speak("What's on my calendar today?")
        handed = wait_for(r"handed off", m, 30)
        check(f"{provider}: the question is handed off", bool(handed))
        # The voice's line comes after the hand-off (xAI says "Let me check." first): wait for it,
        # and for it to be heard, before the goodbye, or the goodbye talks over it.
        said = wait_for(r'(?:Grok|Gemini): "([^"]*)"', m + handed.end(), 30) if handed else None
        check(f"{provider}: the voice says Muse is on it",
              bool(said) and re.search(r"on it|working|let you know|get back|take a (little )?while", said.group(1), re.I),
              said and f'"{said.group(1)[:70]}"')
        if said:
            wait_for(r"reply played", m + handed.end(), 20)
        m2 = speak("Thanks, that's all. Goodbye!")
        over = conversation_over(m2, 40)
        check(f"{provider}: the conversation ends meanwhile", bool(over), over and f"ended ({over.group(1)})")
        back = wait_for(r"Muse got back", m, 120)
        check(f"{provider}: Muse gets back", bool(back), back and back.group(0))
        if back:
            after = back.end()
            played = None
            end = time.time() + 60
            while time.time() < end and not played:
                played = re.search(r"announcement played|reply played", text_from(m)[after:])
                time.sleep(0.3)
            check(f"{provider}: the answer is read out", bool(played), played and played.group(0))
    finally:
        send(b">live_handoff_ms=0\n")   # back to 8 s
        time.sleep(3)


m0 = mark()
send(b">live_provider?\n")
found = wait_for(r"@live_provider (\w+)", m0, 5)
before = found.group(1) if found else "xai"
if args.handoff:
    handoff(before if args.provider in ("both", None) else args.provider)
elif args.all:
    providers = ["gemini", "xai"] if args.provider in ("both", None) else [args.provider]
    for p in providers:
        run(p)
    handoff(providers[0])
    cold_start(providers[0])
else:
    quick(before if args.provider in ("both", None) else args.provider)

whole = text_from(0)
stuck = re.findall(r"voice loop has been stuck[^\n]*", whole)
check("no stuck voice loop", not stuck, "; ".join(stuck[:2]))
mins = [int(x) for x in re.findall(r"min=(\d+)K", whole)]
check("internal RAM never ran out", not mins or min(mins) > 4, mins and f"lowest {min(mins)} K")
errors = re.findall(r"E \(\d+\) [^\n]*", whole)
check("no error lines", not errors, "; ".join(errors[:3]))

send(f">live_provider={before}\n".encode())   # as it was
time.sleep(1)
running = False
save_log()
failed = [n for n, ok, _ in results if not ok]
print(f"\n{len(results) - len(failed)} of {len(results)} passed; the watch's log is in /tmp/e2e_live.log")
sys.exit(1 if failed else 0)
