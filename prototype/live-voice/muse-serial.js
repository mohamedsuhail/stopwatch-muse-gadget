/*
 * Muse through the watch on USB, with Web Serial: the browser's version of the
 * SDK's tools/muse/chat.py. The watch posts a typed turn to Muse
 * (muse_hatch_text_turn) and streams the reply back as "@chat {...}" lines, so
 * Muse keeps your context and nothing here needs a Muse token.
 *
 *   >chat+=TEXT   a piece of the message        -> @chat {"type":"ack","bytes":N}
 *   >chat=TEXT    the last piece, and send it   -> ack, then "sent", "busy",
 *                 "text"/"final" (msg, text), "message_done", "done" (complete) or "error"
 *   >chat.cancel  ends a typed turn
 *   >face=NAME    shows a face: idle listening thinking speaking error off happy
 *   >status       -> @status {"board", "chat", "device"}
 *
 * TEXT escapes \ \n \r \t (muse_hatch_unescape undoes it). The firmware ends a
 * typed turn when the talk button is pressed, and refuses one during a voice turn.
 */
const LINE_BYTES = 480;      /* message bytes a line; escaped, a line stays under the board's 1024 */
const ACK_MS = 3000;
const SEND_MS = 90000;       /* last line -> "sent": includes connecting to Muse */
const TURN_MS = 5 * 60000;   /* a voice prototype gives up well before the board's 15 minutes */
const BOOT_MARKS = ['ESP-ROM:', 'rst:0x'];

export class MuseError extends Error {}

const enc = new TextEncoder();

function escapeBytes(bytes) {
    const out = [];
    for (const b of bytes) {
        if (b === 0x5c) out.push(0x5c, 0x5c);
        else if (b === 0x0a) out.push(0x5c, 0x6e);
        else if (b === 0x0d) out.push(0x5c, 0x72);
        else if (b === 0x09) out.push(0x5c, 0x74);
        else out.push(b);
    }
    return out;
}

/* The console lines that carry `text`, each with the message bytes the board should have once it's in. */
export function splitMessage(text) {
    const data = enc.encode(text.replace(/\0/g, ''));
    const lines = [];
    for (let i = 0; i === 0 || i < data.length; i += LINE_BYTES) {
        const end = Math.min(i + LINE_BYTES, data.length);
        const head = enc.encode(end < data.length ? '>chat+=' : '>chat=');
        lines.push({ bytes: Uint8Array.from([...head, ...escapeBytes(data.subarray(i, end)), 0x0a]), total: end });
    }
    return lines;
}

/* A typed turn's reply, put back together from its "@chat" lines. */
export class Reply {
    constructor() {
        this.parts = new Map();
        this.finals = new Map();
        this.complete = false;
    }

    feed(f) {
        const add = (m) => {
            if (!m.has(f.msg)) m.set(f.msg, []);
            m.get(f.msg).push(f.text || '');
        };
        if (f.type === 'text') add(this.parts);
        else if (f.type === 'final') add(this.finals);
    }

    get text() {
        const msgs = [...new Set([...this.parts.keys(), ...this.finals.keys()])].sort((a, b) => a - b);
        return msgs
            .map((m) => (this.finals.has(m) ? this.finals.get(m) : this.parts.get(m)).join(''))
            .filter(Boolean)
            .join('\n\n');
    }
}

export class MuseSerial {
    /* port: an open SerialPort, or anything with the same readable/writable streams (the tests' fake). */
    constructor(port, log = () => {}) {
        this.port = port;
        this.log = log;
        this.name = 'Muse (watch on USB)';
        this.writer = port.writable.getWriter();
        this.waiters = [];       /* {kind, resolve} for the next "@chat"/"@status" frame */
        this.frames = [];        /* frames nobody was waiting for yet */
        this.recent = [];        /* recent log lines, for error reports */
        this.onRestart = null;
        this.busy = Promise.resolve();
        this.closed = false;
        this.reading = this.readLoop();
    }

    /* Asks for the watch's port (a click is needed) and waits until it answers. */
    static async connect(log) {
        if (!('serial' in navigator)) {
            throw new MuseError('This browser has no Web Serial: use Chrome or Edge');
        }
        const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });   /* Espressif */
        await port.open({ baudRate: 115200 });
        const board = new MuseSerial(port, log);
        log('serial', 'port open; opening it can restart the watch, waiting for it to answer');
        await board.waitReady();
        return board;
    }

    async readLoop() {
        const dec = new TextDecoder();
        let buf = '';
        const reader = this.port.readable.getReader();
        this.reader = reader;
        try {
            for (;;) {
                const { value, done } = await reader.read();
                if (done) break;
                buf += dec.decode(value, { stream: true });
                let i;
                while ((i = buf.indexOf('\n')) >= 0) {
                    this.line(buf.slice(0, i).replace(/\r$/, ''));
                    buf = buf.slice(i + 1);
                }
            }
        } catch (e) {
            if (!this.closed) this.log('serial', 'read stopped: ' + e.message);
        } finally {
            reader.releaseLock();
        }
    }

    line(line) {
        for (const kind of ['@chat', '@status']) {
            const at = line.indexOf(kind + ' {');
            if (at < 0) continue;
            let f;
            try {
                f = JSON.parse(line.slice(at + kind.length + 1));
            } catch {
                this.remember(line);
                return;
            }
            f.kind = kind;
            const w = this.waiters.findIndex((x) => x.kind === kind);
            if (w >= 0) this.waiters.splice(w, 1)[0].resolve(f);
            else this.frames.push(f);
            return;
        }
        if (BOOT_MARKS.some((m) => line.includes(m))) {
            this.log('serial', 'the watch restarted');
            for (const w of this.waiters.splice(0)) w.resolve({ kind: w.kind, type: 'restart' });
            this.onRestart && this.onRestart();
        }
        if (line.trim()) this.remember(line);
    }

    remember(line) {
        this.recent.push(line);
        if (this.recent.length > 30) this.recent.shift();
    }

    recentLog() {
        const tail = this.recent.filter((l) => l.includes('muse_chat') || / [EW] \(/.test(l)).slice(-5);
        return tail.length ? ' Watch log: ' + tail.join(' | ') : '';
    }

    /* The next frame of `kind`, or null after ms. */
    frame(kind, ms) {
        const i = this.frames.findIndex((f) => f.kind === kind);
        if (i >= 0) return Promise.resolve(this.frames.splice(i, 1)[0]);
        return new Promise((resolve) => {
            const w = { kind, resolve };
            this.waiters.push(w);
            setTimeout(() => {
                const j = this.waiters.indexOf(w);
                if (j >= 0) {
                    this.waiters.splice(j, 1);
                    resolve(null);
                }
            }, ms);
        });
    }

    async write(bytes) {
        await this.writer.write(typeof bytes === 'string' ? enc.encode(bytes) : bytes);
    }

    async status(ms = 3000) {
        this.frames = this.frames.filter((f) => f.kind !== '@status');
        await this.write('\n>status\n');   /* the newline ends any half line */
        return this.frame('@status', ms);
    }

    async waitReady(ms = 30000) {
        const end = Date.now() + ms;
        while (Date.now() < end) {
            const st = await this.status(2000);
            if (st && st.type !== 'restart') {
                if (st.chat === false) throw new MuseError(`${st.board} can't chat over serial (no PSRAM)`);
                this.log('serial', `watch answers: ${st.board}`);
                return st;
            }
        }
        throw new MuseError("The watch doesn't answer on serial. Is it running this repo's firmware?");
    }

    /* Mirrors the conversation's state on the watch's own avatar. */
    async face(name) {
        await this.write(`>face=${name}\n`);
    }

    async cancel() {
        await this.write('>chat.cancel\n');
    }

    async expect(types, ms) {
        const end = Date.now() + ms;
        for (;;) {
            const f = await this.frame('@chat', Math.max(0, end - Date.now()));
            if (!f || types.includes(f.type)) return f;
            if (f.type === 'restart') throw new MuseError('The watch restarted.');
            if (f.type === 'error') throw new MuseError('Muse: ' + (f.text || 'error') + this.recentLog());
        }
    }

    async send(text) {
        for (const { bytes, total } of splitMessage(text)) {
            await this.write(bytes);
            const f = await this.expect(['ack'], ACK_MS);
            if (!f || f.bytes !== total) return false;
        }
        return (await this.expect(['sent'], SEND_MS)) !== null;
    }

    /* Sends `text` as a typed turn; resolves to Muse's reply text. One turn at a time. */
    ask(text, progress = () => {}, signal) {
        const run = async () => {
            this.frames = this.frames.filter((f) => f.kind !== '@chat');
            let sent = false;
            for (let attempt = 0; attempt < 3 && !sent; attempt++) {
                sent = await this.send(text);
                if (!sent) await this.cancel();
            }
            if (!sent) throw new MuseError("The watch didn't take the message." + this.recentLog());
            progress({ type: 'sent' });
            const reply = new Reply();
            const end = Date.now() + TURN_MS;
            for (;;) {
                if (signal && signal.aborted) {
                    await this.cancel();
                    throw new MuseError('cancelled');
                }
                const f = await this.frame('@chat', Math.min(1000, Math.max(0, end - Date.now())));
                if (!f) {
                    if (Date.now() >= end) throw new MuseError('The reply stopped coming.' + this.recentLog());
                    continue;
                }
                if (f.type === 'restart') throw new MuseError('The watch restarted.');
                if (f.type === 'error') throw new MuseError('Muse: ' + (f.text || 'error') + this.recentLog());
                reply.feed(f);
                progress(f, reply);
                if (f.type === 'done') {
                    reply.complete = !!f.complete;
                    return reply.text;
                }
            }
        };
        const turn = this.busy.then(run, run);
        this.busy = turn.catch(() => {});
        return turn;
    }

    async close() {
        this.closed = true;
        try {
            await this.reader.cancel();
            await this.reading;
            this.writer.releaseLock();
            await this.port.close();
        } catch {
            /* closing anyway */
        }
    }
}
