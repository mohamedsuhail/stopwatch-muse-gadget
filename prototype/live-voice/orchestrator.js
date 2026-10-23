/*
 * The conversation: Grok (xAI's voice agent) talks, Muse thinks.
 *
 * Grok gets two tools. ask_muse(question) hands anything personal to Muse,
 * which holds your context; set_mood(mood) plays a face clip. When Grok calls
 * ask_muse, this side (on the watch, the firmware) sends the question to Muse
 * and brings the answer back in one of two ways:
 *
 *   wait   'block'  silence (the thinking clip) until Muse answers
 *          'async'  Grok says a short acknowledgement at once and the
 *                   conversation goes on; the answer comes when it's quiet
 *   speak  'verbatim'  Muse's words, cleaned for speech, said in Grok's voice
 *                      with xAI's force_message (no model in between)
 *          'rephrase'  Grok gets the answer as the tool result and says it
 *                      in its own words
 *
 * It needs: rt (XaiRealtime), audio (Audio), muse (MuseSerial or MuseMock),
 * and settings(). It reports through on.* callbacks; app.js draws them.
 */
import { b64decode, b64encode, RATE } from './audio.js';
import { speakable } from './speakable.js';

const MIN_TALK_FRAMES = RATE / 4;   /* a press shorter than 0.25 s is a slip, as on the watch */
export const CONVERSATION_QUIET_MS = 8000;   /* a conversation ends after this long with nothing going on */

export const DEFAULT_STYLE =
    'This message is from my StopWatch, whose voice assistant reads your reply aloud on a small speaker. ' +
    'Answer in one to three short spoken sentences of plain text, like talking. ' +
    'Never number or bullet anything, and use no headings, dashes, emoji or markdown.';

const MOODS = ['joy', 'thinking', 'sad', 'goodnight'];

export function instructions(s) {
    const route = s.routing === 'always'
        ? 'Call ask_muse for every request except greetings, thanks, and asking you to repeat, stop or wait.'
        : 'Call ask_muse for anything personal, to ask or to do: their projects and how they are going; tasks, ' +
          'to-dos and reminders; their calendar, meetings and plans; shopping lists and things to buy; notes and ' +
          'ideas; people, places, preferences and history; anything they told Muse before. That includes ' +
          'requests to add, change or remember something ("add milk to my shopping list", "remind me to ' +
          'flash the watch tomorrow"). When in doubt whether something is personal, call ask_muse. ' +
          'Answer only small talk and general knowledge yourself. Never guess or make up a personal fact.';
    const wait = s.wait === 'async'
        ? 'When you call ask_muse, in the same turn say a short acknowledgement of two to five words, ' +
          'such as "Let me ask Muse." Then stop: Muse\'s answer reaches the user on its own, later.'
        : s.speak === 'verbatim'
            ? 'When you call ask_muse, say nothing before or after it: Muse\'s answer is read to the user word for word.'
            : 'When you call ask_muse, say nothing before it. When its result comes back, tell the user Muse\'s ' +
              'answer faithfully and briefly; don\'t add facts of your own.';
    return [
        'You are MOBOT, a small toy astronaut who lives in the user\'s M5Stack StopWatch and talks for it.',
        'You are the voice in front of Muse, the user\'s personal agent, which holds the personal context of ' +
        'their life and projects. You don\'t have that context yourself.',
        'Speak in one to three short, warm, natural sentences. No lists.',
        route,
        'Write the question or request for ask_muse in the user\'s own words and make it self-contained: include anything ' +
        'from earlier in this conversation that Muse needs to understand it.',
        wait,
        'Call set_mood when the moment clearly fits: joy for good news or a laugh, thinking for a puzzle, sad for ' +
        'bad news, goodnight when the user is leaving or going to sleep. Never mention your tools or Muse\'s ' +
        'internals unless asked.',
    ].join('\n');
}

export function tools() {
    return [
        {
            type: 'function',
            name: 'ask_muse',
            description: 'Hand a question or request to Muse, the user\'s personal assistant, which knows and manages ' +
                'their projects, tasks and to-dos, calendar, shopping lists, notes and history. Use it for anything ' +
                'personal, whether asking or doing. It takes a few seconds.',
            parameters: {
                type: 'object',
                properties: {
                    question: { type: 'string', description: 'The question or request for Muse, self-contained, in the user\'s words.' },
                },
                required: ['question'],
            },
        },
        {
            type: 'function',
            name: 'set_mood',
            description: 'Show a face on the astronaut avatar.',
            parameters: {
                type: 'object',
                properties: { mood: { type: 'string', enum: MOODS } },
                required: ['mood'],
            },
        },
    ];
}

/* Event names: xAI follows OpenAI's GA names; the beta names are accepted too. */
const AUDIO_DELTA = new Set(['response.output_audio.delta', 'response.audio.delta']);
const OUT_TEXT_DELTA = new Set(['response.output_audio_transcript.delta', 'response.audio_transcript.delta']);
const OUT_TEXT_DONE = new Set(['response.output_audio_transcript.done', 'response.audio_transcript.done']);
const IN_TEXT = new Set([
    'conversation.item.input_audio_transcription.completed',
    'conversation.item.input_audio_transcription.updated',
    'conversation.item.input_audio_transcription.delta',
]);
const QUIET = new Set([
    'response.output_audio.done', 'response.audio.done', 'response.content_part.added', 'response.content_part.done',
    'response.output_item.added', 'response.output_item.done', 'conversation.item.added', 'conversation.item.done',
    'conversation.item.created', 'response.function_call_arguments.delta', 'input_audio_buffer.committed',
    'input_audio_buffer.cleared', 'rate_limits.updated', 'ping',
]);

export class Orchestrator {
    constructor({ rt, audio, muse, settings, on = {}, quietMs = CONVERSATION_QUIET_MS }) {
        this.rt = rt;
        this.quietMs = quietMs;
        this.audio = audio;
        this.muse = muse;
        this.settings = settings;
        this.on = Object.assign({
            state() {}, caption() {}, transcript() {}, muse() {}, mark() {}, event() {}, error() {}, mood() {},
        }, on);

        this.talking = false;      /* push-to-talk held (or, hands-free, the server heard speech) */
        this.conversing = false;   /* a conversation started with a long press: hands-free until it ends */
        this.quietTimer = null;    /* ends the conversation after CONVERSATION_QUIET_MS of quiet */
        this.talkFrames = 0;
        this.response = null;      /* the response in progress: {id, calls, audio, cancelled} */
        this.awaiting = false;     /* asked for a response, none started yet */
        this.dropAudio = false;    /* after a barge-in, until the next response starts */
        this.cancelNext = false;   /* barged in after asking for a response that hasn't started */
        this.answerTurn = null;    /* the turn whose Muse answer is about to be said */
        this.cancelled = new Set();
        this.asks = new Map();     /* call_id -> {question, t0, abort} while Muse works */
        this.deliveries = [];      /* Muse answers waiting for a quiet moment */
        this.turn = null;          /* this turn's timings, from letting go of talk */
        this.lastState = '';
        this.outText = '';

        rt.onEvent = (ev) => this.handle(ev);
        audio.onChunk = (pcm, peak) => this.mic(pcm, peak);
        audio.onIdle = () => {
            this.refresh();
            this.deliver();
        };
    }

    /* ---- session ---- */

    configure() {
        const s = this.settings();
        this.rt.send({
            type: 'session.update',
            session: {
                voice: s.voice,
                instructions: instructions(s),
                turn_detection: this.handsFree() ? { type: 'server_vad' } : { type: null },
                audio: {
                    input: { format: { type: 'audio/pcm', rate: RATE } },
                    output: { format: { type: 'audio/pcm', rate: RATE } },
                },
                tools: tools(),
            },
        });
        this.audio.mic(this.handsFree());
        this.refresh();
    }

    /* Hands-free: the setting, or for the length of a conversation. */
    handsFree() {
        return !!this.settings().handsFree || this.conversing;
    }

    /* ---- conversations: long press to start, click (or quiet) to end ----
     *
     * The watch will do the same with its blue button: a long press opens a
     * hands-free conversation, turn after turn by xAI's voice detection, and it
     * closes itself once nobody has spoken, nothing is playing and nothing is
     * waiting on Muse for CONVERSATION_QUIET_MS, or at once on a click. */

    startConversation() {
        if (this.conversing || !this.rt.open) {
            return;
        }
        this.bargeIn();
        this.conversing = true;
        this.talking = false;
        this.turn = null;
        this.rt.send({ type: 'input_audio_buffer.clear' });
        this.configure();   /* server VAD on, mic on */
        this.on.conversation && this.on.conversation(true);
        this.refresh();
    }

    endConversation(why = 'click') {
        if (!this.conversing) {
            return;
        }
        this.conversing = false;
        clearTimeout(this.quietTimer);
        this.quietTimer = null;
        if (why === 'click') {
            this.bargeIn();   /* a click also stops whatever is playing */
        }
        this.talking = false;
        if (this.rt.open) {
            this.rt.send({ type: 'input_audio_buffer.clear' });
            this.configure();   /* back to push-to-talk (or the hands-free setting) */
        }
        this.on.conversation && this.on.conversation(false, why);
        this.refresh();
    }

    /* Restarts the quiet countdown whenever something is going on. */
    watchQuiet() {
        if (!this.conversing) {
            return;
        }
        const busy = this.talking || this.audio.speaking || this.awaiting || this.response
            || this.asks.size || this.deliveries.length;
        if (busy) {
            clearTimeout(this.quietTimer);
            this.quietTimer = null;
        } else if (!this.quietTimer) {
            this.quietTimer = setTimeout(() => {
                this.quietTimer = null;
                this.endConversation('quiet');
            }, this.quietMs);
        }
    }

    /* ---- the talk button ---- */

    press() {
        if (this.talking || !this.rt.open) {
            return;
        }
        this.bargeIn();
        this.talking = true;
        this.talkFrames = 0;
        this.rt.send({ type: 'input_audio_buffer.clear' });
        this.audio.mic(true);
        this.turn = null;
        this.refresh();
    }

    release() {
        if (!this.talking) {
            return;
        }
        this.talking = false;
        if (this.handsFree()) {
            return;
        }
        this.audio.mic(false);
        if (this.talkFrames < MIN_TALK_FRAMES) {
            this.rt.send({ type: 'input_audio_buffer.clear' });
            this.on.caption('Hold to talk');
            this.refresh();
            this.deliver();
            return;
        }
        this.startTurn();
        this.rt.send({ type: 'input_audio_buffer.commit' });
        this.createResponse();
        this.refresh();
    }

    startTurn() {
        this.turn = { t0: performance.now(), marks: {} };
        this.mark('released');
    }

    mic(pcm, peak) {
        this.on.level && this.on.level(peak);
        if (!this.talking && !this.handsFree()) {
            return;
        }
        this.talkFrames += pcm.length;
        this.rt.send({ type: 'input_audio_buffer.append', audio: b64encode(pcm) });
    }

    /* Stops Grok mid-sentence: the speaker goes quiet and the rest of that response is dropped. */
    bargeIn() {
        const speaking = this.audio.speaking;
        this.audio.stop();
        if (this.response && !this.response.cancelled) {
            this.response.cancelled = true;
            this.cancelled.add(this.response.id);
            this.rt.send({ type: 'response.cancel' });
            this.dropAudio = true;
        } else if (this.awaiting) {
            this.cancelNext = true;
            this.awaiting = false;
            this.dropAudio = true;
        } else if (speaking) {
            this.dropAudio = false;   /* only queued audio was left; it's gone */
        }
    }

    createResponse() {
        this.awaiting = true;
        this.rt.send({ type: 'response.create' });
    }

    /* ---- what xAI sends ---- */

    handle(ev) {
        const t = ev.type || '';
        if (AUDIO_DELTA.has(t)) {
            if (this.dropAudio || (ev.response_id && this.cancelled.has(ev.response_id))) {
                return;
            }
            if (this.response) {
                this.response.audio = true;
            }
            if (this.answerTurn) {
                this.mark('answer', 'Muse\'s answer audible', this.answerTurn);
                this.answerTurn = null;
            } else if (this.turn && !this.turn.marks.audio) {
                this.mark('audio', 'first sound');
            }
            this.audio.play(b64decode(ev.delta));
            this.refresh();
            return;
        }
        if (OUT_TEXT_DELTA.has(t)) {
            if (!this.dropAudio) {
                this.outText += ev.delta || '';
                this.on.caption(this.outText);
            }
            return;
        }
        if (OUT_TEXT_DONE.has(t)) {
            if (!this.dropAudio && (ev.transcript || this.outText)) {
                this.on.transcript('mobot', ev.transcript || this.outText);
            }
            this.outText = '';
            return;
        }
        if (IN_TEXT.has(t)) {
            const text = ev.transcript ?? ev.delta ?? '';
            this.on.transcript('you', text, !t.endsWith('.completed'));
            return;
        }
        this.on.event(ev, !QUIET.has(t));
        switch (t) {
        case 'session.created':
        case 'conversation.created':
            break;
        case 'session.updated':
            this.on.mark('session', 'xAI session configured');
            break;
        case 'input_audio_buffer.speech_started':   /* hands-free: the server heard you */
            if (this.handsFree()) {
                this.bargeIn();
                this.talking = true;
                this.turn = null;
                this.refresh();
            }
            break;
        case 'input_audio_buffer.speech_stopped':
            if (this.handsFree()) {
                this.talking = false;
                this.startTurn();
                this.awaiting = true;   /* server VAD makes the response itself */
                this.refresh();
            }
            break;
        case 'response.created':
            this.awaiting = false;
            this.outText = '';
            this.response = { id: ev.response?.id || ev.response_id || 'r' + Date.now(), calls: [], audio: false };
            if (this.cancelNext) {
                /* asked for before a barge-in: not wanted any more */
                this.cancelNext = false;
                this.response.cancelled = true;
                this.cancelled.add(this.response.id);
                this.rt.send({ type: 'response.cancel' });
            } else {
                this.dropAudio = false;
            }
            this.refresh();
            break;
        case 'response.function_call_arguments.done':
            this.call(ev.call_id, ev.name, ev.arguments);
            break;
        case 'response.done':
            this.responseDone(ev.response || {});
            break;
        case 'error':
            this.xaiError(ev);
            break;
        default:
            break;
        }
    }

    xaiError(ev) {
        const msg = ev.error?.message || JSON.stringify(ev.error || ev);
        /* Harmless: cancelling a response that already finished, or nothing left to cancel. */
        if (/cancel|no active response|not.*in progress/i.test(msg)) {
            this.on.event(ev, false);
            return;
        }
        this.awaiting = false;
        this.on.error('xAI: ' + msg);
        this.refresh();
    }

    call(callId, name, args) {
        if (!this.response || this.response.calls.some((c) => c.call_id === callId)) {
            return;
        }
        let parsed = {};
        try {
            parsed = JSON.parse(args || '{}');
        } catch {
            /* left empty */
        }
        this.response.calls.push({ call_id: callId, name, args: parsed });
        if (name === 'set_mood') {
            this.on.mood(parsed.mood);   /* straight away, while Grok may still be speaking */
        }
    }

    responseDone(resp) {
        const r = this.response || { id: resp.id, calls: [] };
        this.response = null;
        /* Calls that only reached response.done (no arguments.done event). */
        for (const item of resp.output || []) {
            if (item.type === 'function_call' && !r.calls.some((c) => c.call_id === item.call_id)) {
                this.response = r;
                this.call(item.call_id, item.name, item.arguments);
                this.response = null;
            }
        }
        const cancelled = r.cancelled || resp.status === 'cancelled';
        let createNext = false;
        for (const c of r.calls) {
            if (c.name === 'set_mood') {
                this.output(c.call_id, { ok: true });
                createNext = true;
            } else if (c.name === 'ask_muse') {
                if (cancelled) {
                    this.output(c.call_id, { error: 'The user interrupted before Muse was asked.' });
                } else {
                    this.askMuse(c.call_id, c.args.question || '');
                }
            } else {
                this.output(c.call_id, { error: 'unknown tool ' + c.name });
            }
        }
        /* A response that only set a mood ends waiting for its tool result: let Grok go on.
         * Not when it also called ask_muse (Muse's answer does that) or was cut off. */
        if (createNext && !cancelled && !r.calls.some((c) => c.name === 'ask_muse') && !r.audio) {
            this.createResponse();
        }
        this.refresh();
        this.deliver();
    }

    output(callId, obj) {
        this.rt.send({
            type: 'conversation.item.create',
            item: { type: 'function_call_output', call_id: callId, output: JSON.stringify(obj) },
        });
    }

    /* ---- Muse ---- */

    askMuse(callId, question) {
        const s = this.settings();
        const abort = new AbortController();
        const ask = { question, t0: performance.now(), abort, turn: this.turn };
        this.asks.set(callId, ask);
        this.mark('tool', 'Grok called ask_muse');
        this.on.muse({ id: callId, phase: 'asked', question });
        this.face('thinking');
        if (s.wait === 'async') {
            /* Answer the tool call now so the conversation can go on; the real answer follows. */
            this.output(callId, {
                status: 'asked',
                note: 'Muse is working on it. Its answer will reach the user by itself; don\'t answer the question yourself.',
            });
        }
        const text = s.style ? `(${s.style})\n\n${question}` : question;
        this.muse.ask(text, (f) => {
            if (f.type === 'sent') {
                this.on.muse({ id: callId, phase: 'sent', ms: performance.now() - ask.t0 });
            } else if (f.type === 'text' || f.type === 'final') {
                this.on.muse({ id: callId, phase: 'text', text: f.text, ms: performance.now() - ask.t0 });
            }
        }, abort.signal).then((answer) => {
            this.asks.delete(callId);
            const ms = performance.now() - ask.t0;
            this.on.muse({ id: callId, phase: 'answer', answer, ms });
            this.mark('muse', `Muse answered in ${(ms / 1000).toFixed(1)} s`, ask.turn);
            this.deliveries.push({ callId, question, answer, wait: s.wait, turn: ask.turn });
            this.deliver();
        }, (e) => {
            this.asks.delete(callId);
            this.on.muse({ id: callId, phase: 'error', error: e.message });
            this.deliveries.push({ callId, question, error: e.message, wait: s.wait, turn: ask.turn });
            this.deliver();
        }).finally(() => this.refresh());
        this.refresh();
    }

    get quiet() {
        return !this.talking && !this.response && !this.awaiting && !this.audio.speaking;
    }

    /* Hands Muse's answers to Grok, one at a time, when nobody is talking. */
    deliver() {
        while (this.deliveries.length && this.quiet) {
            const d = this.deliveries.shift();
            const s = this.settings();
            if (d.wait === 'block') {
                this.output(d.callId, d.error ? { error: 'Muse couldn\'t answer: ' + d.error } : { answer: d.answer });
            }
            if (d.error) {
                if (d.wait === 'block') {
                    this.createResponse();   /* Grok says it couldn't reach Muse */
                } else {
                    this.say('Sorry, I couldn\'t reach Muse just now.');
                }
                break;
            }
            this.answerTurn = d.turn;
            if (s.speak === 'verbatim') {
                this.say(speakable(d.answer));
            } else if (d.wait === 'block') {
                this.createResponse();
            } else {
                this.rt.send({
                    type: 'conversation.item.create',
                    item: {
                        type: 'message',
                        role: 'user',
                        content: [{ type: 'input_text', text: `[Muse's answer to "${d.question}"]: ${d.answer}\nTell me what Muse said.` }],
                    },
                });
                this.createResponse();
            }
            this.on.mark('deliver', `Muse's answer handed to Grok (${s.speak})`);
            break;   /* one at a time: the next waits for this one to be said */
        }
        this.refresh();
    }

    /* Says `text` word for word in Grok's voice (xAI's force_message: no model involved). */
    say(text) {
        this.awaiting = true;
        this.rt.send({
            type: 'conversation.item.create',
            item: { type: 'force_message', role: 'assistant', interruptible: true, content: [{ type: 'output_text', text }] },
        });
    }

    /* Drops Muse questions still out (a new session, or the backend switched). */
    abortAll() {
        for (const a of this.asks.values()) {
            a.abort.abort();
        }
        this.asks.clear();
        this.deliveries = [];
    }

    /* ---- what the avatar shows ---- */

    state() {
        if (!this.rt.open) return 'off';
        if (this.talking) return 'listening';
        if (this.audio.speaking) return 'speaking';
        if (this.awaiting || this.response) return 'thinking';
        if (this.asks.size) return 'muse';
        return this.conversing ? 'listening' : 'idle';   /* in a conversation, it's waiting for you */
    }

    refresh() {
        this.watchQuiet();
        const st = this.state();
        if (st !== this.lastState) {
            this.lastState = st;
            this.on.state(st, this.asks.size);
            this.face({ off: 'idle', muse: 'thinking' }[st] || st);
        }
    }

    face(name) {
        const s = this.settings();
        if (s.mirror && this.muse.face) {
            this.muse.face(name).catch(() => {});
        }
    }

    mark(key, label, turn = this.turn) {
        if (!turn) {
            return;
        }
        if (!(key in turn.marks)) {
            turn.marks[key] = performance.now() - turn.t0;
        }
        this.on.mark(key, label || key, turn.marks[key], turn);
    }
}
