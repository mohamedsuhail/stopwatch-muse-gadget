# Live voice prototype: xAI talks, Muse thinks

A browser prototype of the watch with a **live voice** in front of Muse. You hold a button and talk.
xAI's Voice Agent (Grok, speech to speech) hears you and answers small talk itself. Anything personal
goes to **Muse**, through a tool call, so Muse's context of your projects stays the source of truth.

```
 mic 16 kHz ──► xAI Voice Agent ──► speech 16 kHz ──► speaker
                    │   ▲
       ask_muse(q)  │   │  function_call_output, then force_message (Muse's words)
                    ▼   │
             this page (on the watch: the firmware)
                    │   ▲
     >chat=q (USB)  │   │  @chat {"type":"text"...}
                    ▼   │
               the StopWatch ──► Muse (/chat/stream, your context)
```

It runs on a laptop, but each piece maps onto the watch:

| Piece | Here | On the watch |
| --- | --- | --- |
| Mic and speaker | `audio.js`, `mic-worklet.js`: 16 kHz 16-bit mono both ways | the same rate as the mic and speaker, so nothing is resampled |
| Live voice | `xai-realtime.js`: `wss://api.x.ai/v1/realtime` | the WebSocket client in `muse_xai_tts.c` |
| Auth | `server.py` mints a 10-minute client secret | the stored `>xai_key=` |
| Orchestration | `orchestrator.js` | a new task next to `muse_voice.c` |
| Muse | `muse-serial.js`: typed turns over USB, like the SDK's `tools/muse/chat.py` | `muse_hatch_text_turn()` directly |
| Avatar | the clips in `avatar/character/clips/`, plus `>face=` to mirror the state on the watch | `muse_state_set_mode()`, `muse_state_react()` |

## Run it

Needs Python 3 and Chrome or Edge (Web Serial, to reach the real Muse through the watch).

```bash
XAI_API_KEY=xai-... python3 prototype/live-voice/server.py
# open http://localhost:8765/
```

1. **Connect.** Allow the microphone. The astronaut wakes.
2. **Long-press the button, or Space** (half a second), and talk. That opens a conversation: xAI's
   voice detection ends each turn when you stop, so keep going turn after turn. It closes by itself
   once nothing has happened for 8 s (nobody talking, nothing playing, nothing waiting on Muse), or
   **click** to end it at once. Setup has the old **hold to talk** mode too.
3. Try small talk ("how are you?"), then something only Muse knows ("what's my next task on the StopWatch?").

It starts with a **mock Muse**, which answers from the notes in Setup after 2 to 6 s. To use the
**real Muse**, plug in the watch, open Setup and click **Connect the watch…**. Opening the port
restarts the watch, so give it a few seconds to rejoin Wi-Fi. Questions then go through the watch to
your Muse, and the watch's own avatar follows along (`>face=`).

Keep the key in the server's environment: the browser only ever sees the short-lived token. The page
and server bind to localhost.

## What to try (Setup)

| Setting | Options | What it tests |
| --- | --- | --- |
| Talk button | **Long press** (default): a conversation, ended by a click or 8 s of quiet<br>**Hold to talk**: one turn per press | the conversation flow planned for the watch's blue button |
| While Muse thinks | **Wait quietly** (default): thinking clip until Muse answers<br>**Acknowledge**: Grok says "let me ask Muse" and you can keep talking; the answer comes when it's quiet | whether filler helps or, like the thinking phrases, breaks the flow |
| How Muse's answer is said | **Word for word** (default): Muse's text, cleaned like `speakable()`, in Grok's voice with `force_message`; no model in between<br>**Grok retells it**: Grok gets it as the tool result | fidelity against naturalness |
| What goes to Muse | **Anything personal** (default): projects, tasks and to-dos, calendar, shopping, notes, reminders, asking or doing<br>**Everything** except greetings and "stop" | whether Grok's routing misses personal questions |
| Voice | xAI's voices (`iris` is the watch's TTS voice); hands-free uses xAI's voice detection, with headphones | |
| Style note | sent before each question to Muse, like `MUSE_VOICE_NOTE_STYLE` | |

**Timings** lists every turn, measured from letting go of the button: first sound, when Grok called
`ask_muse`, when Muse answered, and when the answer became audible. **Events** shows the raw xAI events.

## The conversation, for the watch

The talk button follows the plan for the watch's blue button, so it can move to the firmware as is
(`orchestrator.js`: `startConversation`, `endConversation`, `watchQuiet`; `app.js`: the gestures):

```
 IDLE ──long press (0.5 s)──► CONVERSATION ──click──────────────────► IDLE
                              listening ⇄ thinking ⇄ Muse ⇄ speaking
                              └─ 8 s with none of these ─────────────► IDLE
```

- A click while Grok is talking also stops it, so ending is always instant.
- During a conversation the avatar shows listening whenever it's waiting for you.
- `CONVERSATION_QUIET_MS` (8 s) and `LONG_PRESS_MS` (0.5 s) are the two numbers to tune; the
  firmware already uses 0.3 s for its hold-to-wake.

## Tests

```bash
cd prototype/live-voice
npm test       # unit: serial protocol against a fake watch, speech cleanup, every orchestration path
npm run e2e    # headless Chrome + server.py + a fake xAI: one full turn, with a screenshot in test/out/
```

Neither needs network, an xAI key or the watch. The fake xAI covers the protocol as this prototype uses
it. The real API's exact event names were taken from xAI's docs, and the client accepts both the GA
and beta OpenAI-style names. The Events tab shows anything unexpected.

## Known limits

- **Muse's thinking time stays.** A question for Muse still takes Muse's 2 to 6 s. The live voice
  makes everything around it instant, but not Muse itself.
- **Muse sees typed messages,** not voice notes, so the Muse app shows text from the watch.
- **Grok's routing can miss.** In "anything personal" mode, a question that sounds general may be
  answered by Grok without Muse. Use "everything" if that happens too often.
- **Turns Grok answers itself don't reach Muse,** so Muse won't remember them.
- **One Muse question at a time** over USB. More are queued.
- **Ephemeral tokens last 10 minutes** for connecting. Reconnect after a long idle if xAI closes the session.
