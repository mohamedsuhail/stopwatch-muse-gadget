/* muse-mac.js against a fake bridge: no Mac gadget, Muse or network needed. */
import assert from 'node:assert/strict';
import test from 'node:test';

import { MuseMac, sseFrames } from '../muse-mac.js';

/* A fetch that answers /status with `status` and /ask with `chunks`, one stream piece each. */
function fakeBridge({ status = { device: { paired: true, connected: true } }, chunks = [], down = false } = {}) {
    const calls = [];
    const fetchFn = async (url, opts = {}) => {
        calls.push({ url, opts });
        if (down) throw new TypeError('fetch failed');
        if (url.endsWith('/status')) return new Response(JSON.stringify(status), { status: 200 });
        const body = new ReadableStream({
            async pull(c) {
                if (opts.signal?.aborted) return c.error(new DOMException('aborted', 'AbortError'));
                const next = chunks.shift();
                if (next === undefined) return c.close();
                await new Promise((r) => setTimeout(r, 5));
                c.enqueue(new TextEncoder().encode(next));
            },
        });
        return new Response(body, { status: 200, headers: { 'Content-Type': 'text/event-stream' } });
    };
    return { fetchFn, calls };
}

const ev = (f) => 'data: ' + JSON.stringify(f) + '\n\n';

test('sseFrames reads data lines, one frame per event', () => {
    assert.deepEqual([...sseFrames(ev({ type: 'ack' }) + ev({ type: 'sent' }))].map((f) => f.type), ['ack', 'sent']);
});

test('connect needs the Mac paired and connected', async () => {
    const ok = await MuseMac.connect('http://bridge', fakeBridge().fetchFn);
    assert.equal(ok.name, 'Muse (this Mac)');
    await assert.rejects(MuseMac.connect('http://bridge', fakeBridge({ status: { device: { paired: false } } }).fetchFn), /isn't paired/);
    await assert.rejects(MuseMac.connect('http://bridge', fakeBridge({ status: { device: { paired: true, connected: false } } }).fetchFn), /not connected/);
    await assert.rejects(MuseMac.connect('http://bridge', fakeBridge({ down: true }).fetchFn), /isn't running/);
});

test('a turn: frames split across chunks, progress for each, the reply text at done', async () => {
    const all = ev({ seq: 1, type: 'ack', bytes: 9 }) + ev({ seq: 2, type: 'sent', bytes: 9 }) + ev({ seq: 3, type: 'busy', on: true })
        + ev({ seq: 4, type: 'text', msg: 0, text: 'You have ' }) + ev({ seq: 5, type: 'text', msg: 0, text: 'two meetings.' })
        + ev({ seq: 6, type: 'message_done', msg: 0, bytes: 22 }) + ev({ seq: 7, type: 'done', messages: 1, complete: true });
    const chunks = [all.slice(0, 50), all.slice(50, 51), all.slice(51, 170), all.slice(170)];
    const { fetchFn, calls } = fakeBridge({ chunks });
    const seen = [];
    const answer = await new MuseMac('http://bridge', fetchFn).ask('What is on today?', (f) => seen.push(f.type));
    assert.equal(answer, 'You have two meetings.');
    assert.deepEqual(seen, ['ack', 'sent', 'busy', 'text', 'text', 'message_done', 'done']);
    assert.equal(calls[0].url, 'http://bridge/ask');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { text: 'What is on today?' });
});

test('an error frame rejects with Muse\'s reason', async () => {
    const { fetchFn } = fakeBridge({ chunks: [ev({ type: 'ack', bytes: 2 }) + ev({ type: 'error', text: 'MUSE NOT CONNECTED' })] });
    await assert.rejects(new MuseMac('http://bridge', fetchFn).ask('hi'), /Muse: MUSE NOT CONNECTED/);
});

test('a reply that stops before done is an error, and aborting cancels', async () => {
    const early = fakeBridge({ chunks: [ev({ type: 'ack', bytes: 2 })] });
    await assert.rejects(new MuseMac('http://bridge', early.fetchFn).ask('hi'), /closed the reply early/);
    const ctl = new AbortController();
    const slow = fakeBridge({ chunks: [ev({ type: 'ack', bytes: 2 }), ev({ type: 'sent', bytes: 2 }), ev({ type: 'busy', on: true })] });
    const turn = new MuseMac('http://bridge', slow.fetchFn).ask('hi', () => ctl.abort(), ctl.signal);
    await assert.rejects(turn, /cancelled/);
});
