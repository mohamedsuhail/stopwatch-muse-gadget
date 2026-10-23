/*
 * cd prototype/live-voice && npm test
 *
 * The serial protocol against a fake watch, the speech cleanup, and the
 * orchestrator's tool-call flows against a scripted xAI. No network, no browser.
 */
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { MuseMock } from '../muse-mock.js';
import { MuseSerial, splitMessage } from '../muse-serial.js';
import { Orchestrator, instructions } from '../orchestrator.js';
import { speakable } from '../speakable.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---- speakable ---- */

test('speakable drops list marks, markdown and emoji, and turns dashes into pauses', () => {
    assert.equal(speakable('1. **First** thing\n2) second — really 🚀\n- third\n• fourth  \n'),
        'First thing\nsecond, really \nthird\nfourth');
    assert.equal(speakable('Version 3.5 is out'), 'Version 3.5 is out');
});

/* ---- serial ---- */

test('splitMessage escapes and splits on 480 message bytes, like chat.py', () => {
    const [one] = splitMessage('a\\b\nc');
    assert.equal(new TextDecoder().decode(one.bytes), '>chat=a\\\\b\\nc\n');
    assert.equal(one.total, 5);
    const lines = splitMessage('x'.repeat(1000));
    assert.deepEqual(lines.map((l) => l.total), [480, 960, 1000]);
    assert.ok(new TextDecoder().decode(lines[0].bytes).startsWith('>chat+='));
    assert.ok(new TextDecoder().decode(lines[2].bytes).startsWith('>chat='));
});

/* A watch on a fake serial port: answers status, acks chat lines, streams a reply. */
function fakeWatch(reply, { failFirst = false } = {}) {
    let toHost;
    const got = [];
    let message = '';
    let sentOnce = false;
    const say = (s) => toHost.enqueue(new TextEncoder().encode(s));
    const port = {
        readable: new ReadableStream({ start(c) { toHost = c; } }),
        writable: new WritableStream({
            write(chunk) {
                for (const line of new TextDecoder().decode(chunk).split('\n')) {
                    if (!line) continue;
                    got.push(line);
                    if (line === '>status') {
                        say('I (123) muse: some log line\r\n@status {"board":"stopwatch","chat":true,"device":{}}\r\n');
                    } else if (line.startsWith('>chat+=') || line.startsWith('>chat=')) {
                        const last = line.startsWith('>chat=');
                        message += line.slice(last ? 6 : 7).replace(/\\n/g, '\n').replace(/\\\\/g, '\\');
                        say(`@chat {"seq":1,"type":"ack","bytes":${new TextEncoder().encode(message).length}}\n`);
                        if (last) {
                            if (failFirst && !sentOnce) {
                                sentOnce = true;
                                say('@chat {"seq":2,"type":"error","text":"BUSY"}\n');
                                message = '';
                                continue;
                            }
                            setTimeout(() => {
                                say('@chat {"seq":2,"type":"sent","bytes":1}\n@chat {"seq":3,"type":"busy","on":true}\n');
                                const half = Math.floor(reply.length / 2);
                                say(`@chat {"seq":4,"type":"text","msg":0,"text":${JSON.stringify(reply.slice(0, half))}}\n`);
                                say(`noise @chat {"seq":5,"type":"text","msg":0,"text":${JSON.stringify(reply.slice(half))}}\n`);
                                say('@chat {"seq":6,"type":"message_done","msg":0,"bytes":1}\n@chat {"seq":7,"type":"done","complete":true}\n');
                            }, 20);
                            message = '';
                        }
                    }
                }
            },
        }),
        close: async () => {},
    };
    return { port, got };
}

test('MuseSerial: status, then a typed turn streamed back', async () => {
    const { port, got } = fakeWatch('Your next task is moving the SDK out of iCloud.');
    const board = new MuseSerial(port);
    const st = await board.waitReady(2000);
    assert.equal(st.board, 'stopwatch');
    const seen = [];
    const answer = await board.ask('What is next?\nThanks', (f) => seen.push(f.type));
    assert.equal(answer, 'Your next task is moving the SDK out of iCloud.');
    assert.deepEqual(seen.filter((t) => t !== 'busy'), ['sent', 'text', 'text', 'message_done', 'done']);
    assert.ok(got.includes('>chat=What is next?\\nThanks'));
    await board.face('thinking');
    assert.equal(got.at(-1), '>face=thinking');
    await board.close();
});

test('MuseSerial: a Muse error rejects the turn, and the next turn still works', async () => {
    const { port } = fakeWatch('ok', { failFirst: true });
    const board = new MuseSerial(port);
    await assert.rejects(board.ask('hi'), /Muse: BUSY/);
    assert.equal(await board.ask('hi again'), 'ok');
    await board.close();
});

/* ---- orchestrator ---- */

/* xAI, scripted: records what the page sends; the test plays the server's part. */
class FakeRt {
    constructor() {
        this.sent = [];
        this.open = true;
        this.onEvent = () => {};
    }
    send(ev) {
        this.sent.push(ev);
        return true;
    }
    types() {
        return this.sent.map((e) => e.type);
    }
    emit(...evs) {
        for (const e of evs) this.onEvent(e);
    }
}

class FakeAudio {
    constructor() {
        this.played = 0;
        this.speaking = false;
        this.micOn = false;
    }
    play(bytes) {
        this.played += bytes.length;
        this.speaking = true;
    }
    finish() {
        this.speaking = false;
        this.onIdle && this.onIdle();
    }
    stop() {
        this.speaking = false;
    }
    mic(on) {
        this.micOn = on;
    }
}

function rig(over = {}) {
    const rt = new FakeRt();
    const audio = new FakeAudio();
    const s = { wait: 'block', speak: 'verbatim', routing: 'personal', voice: 'iris', handsFree: false, style: 'STYLE', mirror: false, ...over };
    const muse = new MuseMock(() => 'The StopWatch next task is moving the SDK checkout out of iCloud.', { minMs: 30, maxMs: 40 });
    const asked = [];
    const realAsk = muse.ask.bind(muse);
    muse.ask = (text, ...rest) => {
        asked.push(text);
        return realAsk(text, ...rest);
    };
    const states = [];
    const orch = new Orchestrator({ rt, audio, muse, settings: () => s, on: { state: (st) => states.push(st) } });
    return { rt, audio, orch, asked, states, s };
}

/* A spoken turn: press, enough audio, release. */
function talk(rt, orch) {
    orch.press();
    orch.mic(new Int16Array(16000 / 2), 0.5);   /* half a second */
    orch.release();
}

const audioDelta = (id) => ({ type: 'response.output_audio.delta', response_id: id, delta: Buffer.from(new Uint8Array(640)).toString('base64') });

test('session.update asks for 16 kHz PCM both ways, push-to-talk, and both tools', () => {
    const { rt, orch } = rig();
    orch.configure();
    const u = rt.sent[0];
    assert.equal(u.type, 'session.update');
    assert.deepEqual(u.session.turn_detection, { type: null });
    assert.equal(u.session.audio.input.format.rate, 16000);
    assert.equal(u.session.audio.output.format.rate, 16000);
    assert.equal(u.session.voice, 'iris');
    assert.deepEqual(u.session.tools.map((t) => t.name), ['ask_muse', 'set_mood']);
    assert.match(instructions({ routing: 'always', wait: 'async', speak: 'verbatim' }), /every request/);
});

test('a short press is ignored; a real one commits and asks for a response', () => {
    const { rt, orch } = rig();
    orch.press();
    orch.mic(new Int16Array(1000), 0.1);
    orch.release();
    assert.deepEqual(rt.types(), ['input_audio_buffer.clear', 'input_audio_buffer.append', 'input_audio_buffer.clear']);
    rt.sent = [];
    talk(rt, orch);
    assert.deepEqual(rt.types().slice(-2), ['input_audio_buffer.commit', 'response.create']);
});

test('block + verbatim: Muse is asked, then its answer is said word for word', async () => {
    const { rt, audio, orch, asked, states } = rig();
    talk(rt, orch);
    rt.sent = [];
    rt.emit(
        { type: 'response.created', response: { id: 'r1' } },
        { type: 'response.function_call_arguments.done', call_id: 'c1', name: 'ask_muse', arguments: '{"question":"What is my next StopWatch task?"}' },
        { type: 'response.done', response: { id: 'r1', status: 'completed' } },
    );
    assert.equal(states.at(-1), 'muse');
    assert.equal(rt.sent.length, 0, 'nothing goes back until Muse answers');
    await sleep(450);
    assert.equal(asked.length, 1);
    assert.match(asked[0], /^\(STYLE\)\n\nWhat is my next StopWatch task\?/);
    const [out, force] = rt.sent;
    assert.equal(out.item.type, 'function_call_output');
    assert.equal(out.item.call_id, 'c1');
    assert.match(JSON.parse(out.item.output).answer, /iCloud/);
    assert.equal(force.item.type, 'force_message');
    assert.match(force.item.content[0].text, /iCloud/);
    assert.ok(!rt.types().includes('response.create'), 'verbatim needs no model turn');
    /* xAI plays the forced message as a normal response */
    rt.emit({ type: 'response.created', response: { id: 'r2' } }, audioDelta('r2'));
    assert.equal(states.at(-1), 'speaking');
    assert.ok(audio.played > 0);
    assert.ok(orch.turn.marks.answer >= 0, 'the answer’s first sound is timed');
});

test('block + rephrase: the answer goes back as the tool result and Grok says it', async () => {
    const { rt, orch } = rig({ speak: 'rephrase' });
    talk(rt, orch);
    rt.sent = [];
    rt.emit(
        { type: 'response.created', response: { id: 'r1' } },
        { type: 'response.function_call_arguments.done', call_id: 'c1', name: 'ask_muse', arguments: '{"question":"next task?"}' },
        { type: 'response.done', response: { id: 'r1' } },
    );
    await sleep(450);
    assert.deepEqual(rt.types(), ['conversation.item.create', 'response.create']);
    assert.equal(rt.sent[0].item.type, 'function_call_output');
});

test('async: the tool call is answered at once, and Muse’s answer waits for a quiet moment', async () => {
    const { rt, audio, orch } = rig({ wait: 'async' });
    talk(rt, orch);
    rt.sent = [];
    rt.emit(
        { type: 'response.created', response: { id: 'r1' } },
        audioDelta('r1'),   /* "Let me ask Muse." */
        { type: 'response.function_call_arguments.done', call_id: 'c1', name: 'ask_muse', arguments: '{"question":"next task?"}' },
        { type: 'response.done', response: { id: 'r1' } },
    );
    assert.equal(rt.sent[0].item.type, 'function_call_output');
    assert.equal(JSON.parse(rt.sent[0].item.output).status, 'asked');
    /* the user talks again while Muse works */
    orch.press();
    await sleep(450);
    assert.ok(!rt.sent.some((e) => e.item?.type === 'force_message'), 'not while the user is talking');
    orch.mic(new Int16Array(8000), 0.5);
    orch.release();
    rt.emit({ type: 'response.created', response: { id: 'r2' } }, audioDelta('r2'), { type: 'response.done', response: { id: 'r2' } });
    assert.ok(!rt.sent.some((e) => e.item?.type === 'force_message'), 'not while Grok is speaking');
    audio.finish();
    assert.ok(rt.sent.some((e) => e.item?.type === 'force_message'), 'said once it is quiet');
});

test('barge-in cancels the response, drops its audio, and a cut-off ask_muse is closed', () => {
    const { rt, audio, orch } = rig();
    talk(rt, orch);
    rt.emit({ type: 'response.created', response: { id: 'r1' } }, audioDelta('r1'));
    const before = audio.played;
    rt.sent = [];
    orch.press();
    assert.ok(rt.types().includes('response.cancel'));
    rt.emit(
        audioDelta('r1'),
        { type: 'response.function_call_arguments.done', call_id: 'c9', name: 'ask_muse', arguments: '{"question":"x"}' },
        { type: 'response.done', response: { id: 'r1', status: 'cancelled' } },
    );
    assert.equal(audio.played, before, 'late audio of the cancelled response is dropped');
    const out = rt.sent.find((e) => e.item?.type === 'function_call_output');
    assert.match(JSON.parse(out.item.output).error, /interrupted/);
});

test('set_mood alone gets its result and a new response; with speech it needs none', () => {
    const moods = [];
    const { rt, orch } = rig();
    orch.on.mood = (m) => moods.push(m);
    talk(rt, orch);
    rt.sent = [];
    rt.emit(
        { type: 'response.created', response: { id: 'r1' } },
        { type: 'response.function_call_arguments.done', call_id: 'm1', name: 'set_mood', arguments: '{"mood":"joy"}' },
        { type: 'response.done', response: { id: 'r1' } },
    );
    assert.deepEqual(moods, ['joy']);
    assert.deepEqual(rt.types(), ['conversation.item.create', 'response.create']);
    rt.sent = [];
    rt.emit(
        { type: 'response.created', response: { id: 'r2' } },
        audioDelta('r2'),
        { type: 'response.function_call_arguments.done', call_id: 'm2', name: 'set_mood', arguments: '{"mood":"goodnight"}' },
        { type: 'response.done', response: { id: 'r2' } },
    );
    assert.deepEqual(rt.types(), ['conversation.item.create']);
});

test('a function call only listed in response.done is still run', async () => {
    const { rt, orch, asked } = rig();
    talk(rt, orch);
    rt.emit(
        { type: 'response.created', response: { id: 'r1' } },
        { type: 'response.done', response: { id: 'r1', output: [{ type: 'function_call', call_id: 'c1', name: 'ask_muse', arguments: '{"question":"q"}' }] } },
    );
    await sleep(450);
    assert.equal(asked.length, 1);
});
