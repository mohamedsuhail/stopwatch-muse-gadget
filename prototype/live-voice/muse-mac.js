/*
 * Muse through this Mac as a gadget of its own: prototype/muse-gadget-mac's bridge
 * (`uv run python -m musemac run`) on 127.0.0.1:8766, so the real Muse is reachable
 * without the watch.
 *
 * The bridge speaks the watch's typed-turn protocol over HTTP: POST /ask streams the
 * same frames the watch prints after "@chat" (ack, sent, busy, text, final,
 * message_done, done, error) as server-sent events. So a turn here is read with the
 * watch's own Reply, and the page's conversation code doesn't care which Muse it has.
 */
import { MuseError, Reply } from './muse-serial.js';

export const MAC_BRIDGE = 'http://127.0.0.1:8766';
const STATUS_MS = 3000;

/* The frames in an event stream's text: "data: {...}" lines, events split by a blank line. */
export function* sseFrames(text) {
    for (const event of text.split(/\r?\n\r?\n/)) {
        const data = event.split(/\r?\n/).filter((l) => l.startsWith('data:')).map((l) => l.slice(5).trim()).join('\n');
        if (data) {
            yield JSON.parse(data);
        }
    }
}

export class MuseMac {
    constructor(base = MAC_BRIDGE, fetchFn = (...a) => fetch(...a)) {
        this.base = base;
        this.fetch = fetchFn;
        this.name = 'Muse (this Mac)';
    }

    /* Checks the bridge is up and the Mac is registered with Muse; resolves to a MuseMac. */
    static async connect(base, fetchFn) {
        const mac = new MuseMac(base, fetchFn);
        const st = await mac.status();
        if (!st.device?.paired) {
            throw new MuseError('this Mac isn\'t paired with Muse yet: run `uv run python -m musemac pair` in prototype/muse-gadget-mac');
        }
        if (!st.device?.connected) {
            throw new MuseError('the Mac gadget is running but not connected to Muse yet: give it a few seconds, then connect again');
        }
        return mac;
    }

    async status() {
        let res;
        try {
            res = await this.fetch(this.base + '/status', { signal: AbortSignal.timeout(STATUS_MS) });
        } catch {
            throw new MuseError('the Mac gadget isn\'t running: click Start the Mac gadget in Setup');
        }
        if (!res.ok) {
            throw new MuseError(`the Mac gadget answered HTTP ${res.status}`);
        }
        return res.json();
    }

    /* Sends `text` as a typed turn; resolves to Muse's reply text. Aborting `signal` ends the turn. */
    async ask(text, progress = () => {}, signal) {
        let res;
        try {
            res = await this.fetch(this.base + '/ask', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ text }),
                signal,
            });
        } catch (e) {
            throw new MuseError(signal?.aborted ? 'cancelled' : 'the Mac gadget isn\'t running: click Start the Mac gadget in Setup');
        }
        if (!res.ok) {
            throw new MuseError(`the Mac gadget answered HTTP ${res.status}`);
        }
        const reply = new Reply();
        const reader = res.body.pipeThrough(new TextDecoderStream()).getReader();
        let buf = '';
        try {
            for (;;) {
                const { value, done } = await reader.read();
                if (done) {
                    throw new MuseError('the Mac gadget closed the reply early');
                }
                buf += value;
                const cut = buf.lastIndexOf('\n\n');
                if (cut < 0) {
                    continue;
                }
                const ready = buf.slice(0, cut);
                buf = buf.slice(cut + 2);
                for (const f of sseFrames(ready)) {
                    if (f.type === 'error') {
                        throw new MuseError('Muse: ' + (f.text || 'error'));
                    }
                    reply.feed(f);
                    progress(f, reply);
                    if (f.type === 'done') {
                        reply.complete = !!f.complete;
                        return reply.text;
                    }
                }
            }
        } catch (e) {
            if (signal?.aborted) {
                throw new MuseError('cancelled');
            }
            throw e instanceof MuseError ? e : new MuseError('the reply from the Mac gadget broke off: ' + e.message);
        } finally {
            reader.cancel().catch(() => {});
        }
    }

    async face() {}   /* no avatar of its own */

    async close() {}
}
