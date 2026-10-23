/*
 * End to end in headless Chrome, with a fake xAI:  cd prototype/live-voice && npm run e2e
 *
 * Starts a fake xAI (client_secrets over HTTP, the realtime WebSocket), runs
 * server.py against it, opens the page in Chrome with a fake microphone,
 * holds Space for a turn and checks the whole loop: token -> session.update ->
 * mic audio -> commit -> Grok calls ask_muse -> mock Muse answers ->
 * function_call_output + force_message -> speech plays -> the page shows it.
 * Leaves a screenshot in test/out/. Needs Chrome (CHROME=path to override).
 * No npm packages: Node's own WebSocket client drives Chrome over CDP.
 */
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const XAI_PORT = 9401;
const APP_PORT = 8799;
const CDP_PORT = 9333;
const CHROME = process.env.CHROME || 'google-chrome';
const TOKEN = 'eph-test-token';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => {
    throw new Error(msg);
};

/* ---- a fake xAI ---- */

const seen = { tokenAuth: null, protocol: null, session: null, appendBytes: 0, types: [], outputs: [], forced: [] };

function frame(text) {
    const data = Buffer.from(text);
    const n = data.length;
    const head = n < 126 ? Buffer.from([0x81, n])
        : n < 65536 ? Buffer.from([0x81, 126, n >> 8, n & 255])
            : Buffer.concat([Buffer.from([0x81, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(n)); return b; })()]);
    return Buffer.concat([head, data]);
}

/* Unmasks the client's frames; calls onText for each text message. */
function reader(onText) {
    let buf = Buffer.alloc(0);
    return (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        for (;;) {
            if (buf.length < 2) return;
            const op = buf[0] & 15;
            let n = buf[1] & 127;
            let at = 2;
            if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); at = 4; }
            else if (n === 127) { if (buf.length < 10) return; n = Number(buf.readBigUInt64BE(2)); at = 10; }
            if (buf.length < at + 4 + n) return;
            const mask = buf.subarray(at, at + 4);
            const data = Buffer.from(buf.subarray(at + 4, at + 4 + n));
            for (let i = 0; i < n; i++) data[i] ^= mask[i & 3];
            buf = buf.subarray(at + 4 + n);
            if (op === 1) onText(data.toString());
        }
    };
}

function tone(ms) {
    const n = Math.round(16 * ms);
    const pcm = new Int16Array(n);
    for (let i = 0; i < n; i++) pcm[i] = Math.round(6000 * Math.sin((2 * Math.PI * 440 * i) / 16000));
    return Buffer.from(pcm.buffer).toString('base64');
}

function fakeXai() {
    const srv = http.createServer((req, res) => {
        if (req.method === 'POST' && req.url === '/v1/realtime/client_secrets') {
            seen.tokenAuth = req.headers.authorization;
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ value: TOKEN, expires_at: 0 }));
        } else if (req.url === '/v1/tts/voices') {
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ voices: [{ voice_id: 'iris' }, { voice_id: 'eve' }, { voice_id: 'ara' }] }));
        } else {
            res.writeHead(404);
            res.end();
        }
    });
    srv.on('upgrade', (req, sock) => {
        seen.protocol = req.headers['sec-websocket-protocol'];
        const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
            `Sec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: ${seen.protocol}\r\n\r\n`);
        const send = (ev) => sock.write(frame(JSON.stringify(ev)));
        let n = 0;
        /* One spoken response: transcript, audio, done. */
        const speak = (id, text, ms) => {
            send({ type: 'response.created', response: { id } });
            send({ type: 'response.output_audio_transcript.delta', response_id: id, delta: text });
            for (let t = 0; t < ms; t += 200) send({ type: 'response.output_audio.delta', response_id: id, delta: tone(200) });
            send({ type: 'response.output_audio_transcript.done', response_id: id, transcript: text });
            send({ type: 'response.done', response: { id, status: 'completed' } });
        };
        send({ type: 'session.created', session: {} });
        sock.on('data', reader((text) => {
            const ev = JSON.parse(text);
            seen.types.push(ev.type);
            if (ev.type === 'session.update') {
                seen.session = ev.session;
                send({ type: 'session.updated', session: ev.session });
            } else if (ev.type === 'input_audio_buffer.append') {
                seen.appendBytes += Buffer.from(ev.audio, 'base64').length;
            } else if (ev.type === 'response.create') {
                const id = 'resp_' + ++n;
                send({ type: 'conversation.item.input_audio_transcription.completed', transcript: "What's my next StopWatch task?" });
                send({ type: 'response.created', response: { id } });
                send({ type: 'response.function_call_arguments.done', response_id: id, call_id: 'call_1', name: 'ask_muse',
                    arguments: JSON.stringify({ question: "What's my next task on the StopWatch project?" }) });
                send({ type: 'response.done', response: { id, status: 'completed' } });
            } else if (ev.type === 'conversation.item.create' && ev.item.type === 'function_call_output') {
                seen.outputs.push(JSON.parse(ev.item.output));
            } else if (ev.type === 'conversation.item.create' && ev.item.type === 'force_message') {
                seen.forced.push(ev.item.content[0].text);
                setTimeout(() => speak('resp_' + ++n, ev.item.content[0].text, 1200), 50);
            }
        }));
        sock.on('error', () => {});
    });
    return new Promise((r) => srv.listen(XAI_PORT, '127.0.0.1', () => r(srv)));
}

/* ---- Chrome over CDP ---- */

async function cdp(url) {
    const ws = new WebSocket(url);
    await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
    let id = 0;
    const pending = new Map();
    const listeners = [];
    ws.onmessage = (e) => {
        const m = JSON.parse(e.data);
        if (m.id && pending.has(m.id)) {
            const { resolve, reject } = pending.get(m.id);
            pending.delete(m.id);
            m.error ? reject(new Error(m.error.message)) : resolve(m.result);
        } else if (m.method) {
            for (const l of listeners) l(m);
        }
    };
    return {
        send: (method, params = {}) => new Promise((resolve, reject) => {
            pending.set(++id, { resolve, reject });
            ws.send(JSON.stringify({ id, method, params }));
        }),
        on: (fn) => listeners.push(fn),
        close: () => ws.close(),
    };
}

async function main() {
    const procs = [];
    const xai = await fakeXai();
    const app = spawn('python3', [path.join(HERE, '..', 'server.py'), '--port', String(APP_PORT)], {
        env: { ...process.env, XAI_API_KEY: 'test-key', XAI_API_BASE: `http://127.0.0.1:${XAI_PORT}`, XAI_WS_URL: `ws://127.0.0.1:${XAI_PORT}/v1/realtime` },
        stdio: ['ignore', 'ignore', 'pipe'],
    });
    procs.push(app);
    let appLog = '';
    app.stderr.on('data', (d) => { appLog += d; });
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'mobot-e2e-'));
    const chrome = spawn(CHROME, [
        '--headless=new', `--remote-debugging-port=${CDP_PORT}`, `--user-data-dir=${profile}`,
        '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required',
        '--no-first-run', '--no-default-browser-check', '--window-size=1280,860', 'about:blank',
    ], { stdio: 'ignore' });
    procs.push(chrome);
    const errors = [];
    try {
        let version;
        for (let i = 0; i < 50 && !version; i++) {
            await sleep(200);
            version = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`).then((r) => r.json()).catch(() => null);
        }
        const page = version?.find((t) => t.type === 'page') || fail('Chrome gave no page');
        const c = await cdp(page.webSocketDebuggerUrl);
        c.on((m) => {
            if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
            if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
            if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error' && !/favicon/.test(m.params.entry.url || '')) errors.push(m.params.entry.text + ' ' + (m.params.entry.url || ''));
        });
        await c.send('Runtime.enable');
        await c.send('Log.enable');
        await c.send('Page.enable');
        await c.send('Page.navigate', { url: `http://localhost:${APP_PORT}/` });
        await sleep(1500);
        const js = async (expr) => (await c.send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true, userGesture: true })).result.value;

        await js("localStorage.clear(); document.getElementById('connect').click()");
        for (let i = 0; i < 30 && !(await js("document.getElementById('xaiPill').textContent")).includes('live'); i++) await sleep(200);
        const pill = await js("document.getElementById('xaiPill').textContent");
        if (!pill.includes('live')) fail('xAI never connected: ' + pill + ' ' + (await js("document.getElementById('log').innerText")));
        await sleep(600);

        /* Hold Space for 1.2 s: push-to-talk. */
        const key = (type) => c.send('Input.dispatchKeyEvent', { type, code: 'Space', key: ' ', windowsVirtualKeyCode: 32 });
        await key('keyDown');
        await sleep(400);
        const listening = await js("document.getElementById('screen').dataset.state");
        await sleep(800);
        await key('keyUp');

        /* Muse (mock) takes 2 to 6 s, then the forced answer plays. */
        let states = new Set();
        for (let i = 0; i < 60; i++) {
            states.add(await js("document.getElementById('screen').dataset.state"));
            if (seen.forced.length && states.has('speaking') && (await js("document.getElementById('screen').dataset.state")) === 'idle') break;
            await sleep(200);
        }
        const shot = await c.send('Page.captureScreenshot', { format: 'png' });
        fs.mkdirSync(path.join(HERE, 'out'), { recursive: true });
        fs.writeFileSync(path.join(HERE, 'out', 'e2e.png'), Buffer.from(shot.data, 'base64'));
        const conversation = await js("document.getElementById('log').innerText");
        const times = await js("[...document.querySelectorAll('#times tr')].map(r => [...r.cells].map(c => c.textContent).join(' | ')).join('\\n')");
        const voices = await js("[...document.getElementById('voice').options].map(o => o.value).join(',')");
        c.close();

        /* ---- checks ---- */
        const checks = [
            ['server.py minted the token with the API key', seen.tokenAuth === 'Bearer test-key'],
            ['the browser passed the token as the subprotocol', seen.protocol === 'xai-client-secret.' + TOKEN],
            ['session.update: 16 kHz PCM, push-to-talk, voice iris, both tools',
                seen.session?.audio?.input?.format?.rate === 16000 && seen.session?.turn_detection?.type === null &&
                seen.session?.voice === 'iris' && seen.session?.tools?.map((t) => t.name).join() === 'ask_muse,set_mood'],
            ['the avatar showed listening while Space was held', listening === 'listening'],
            ['the fake mic streamed about 1.2 s of 16 kHz audio', seen.appendBytes > 16000 * 2 * 0.8],
            ['commit, then response.create', seen.types.indexOf('input_audio_buffer.commit') < seen.types.indexOf('response.create') && seen.types.includes('input_audio_buffer.commit')],
            ['Muse was asked and the page showed it', /ask_muse → Mock Muse/i.test(conversation) && /next task on the StopWatch/.test(conversation)],
            ['the asking-Muse state showed', states.has('muse')],
            ['the tool result carries Muse’s answer', /iCloud/.test(seen.outputs[0]?.answer || '')],
            ['Muse’s answer was forced word for word', /iCloud/.test(seen.forced[0] || '')],
            ['the answer played (speaking state)', states.has('speaking')],
            ['transcripts shown for both sides', /YOU\s*\n?What's my next StopWatch task\?/i.test(conversation) && /MOBOT/i.test(conversation)],
            ['timings recorded (ask_muse, Muse answered, answer audible)', /\d\.\d\d s \| \d\.\d\d s \| \d\.\d\d s/.test(times)],
            ['voices listed through server.py', voices.includes('ara')],
            ['no errors in the page', errors.length === 0],
        ];
        let bad = 0;
        for (const [name, ok] of checks) {
            console.log((ok ? 'ok   ' : 'FAIL ') + name);
            bad += !ok;
        }
        console.log('\nTimings (turn | first sound | ask_muse | Muse answered | answer audible):\n' + times);
        console.log('\nConversation panel:\n' + conversation);
        if (errors.length) console.log('\nPage errors:\n' + errors.join('\n'));
        if (bad) {
            console.log('\nserver.py log:\n' + appLog);
            process.exitCode = 1;
        }
        console.log('\nScreenshot: ' + path.join(HERE, 'out', 'e2e.png'));
    } finally {
        for (const p of procs) p.kill();
        xai.close();
        xai.closeAllConnections?.();
        await sleep(300);
        fs.rmSync(profile, { recursive: true, force: true });
    }
}

main().catch((e) => {
    console.error(e);
    process.exit(1);
});
