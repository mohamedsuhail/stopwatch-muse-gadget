/*
 * Mic and speaker, both 16 kHz 16-bit mono like the watch: the session asks
 * xAI for that rate each way, so nothing is resampled here either.
 */
export const RATE = 16000;

export class Audio {
    constructor() {
        this.ctx = null;
        this.node = null;
        this.onChunk = null;     /* (Int16Array, peak) while the mic is on */
        this.playing = new Set();
        this.nextAt = 0;
        this.onIdle = null;      /* the last queued speech has finished */
        this.played = 0;         /* frames queued since the last stop() */
    }

    async start() {
        if (this.ctx) {
            return;
        }
        /* Chrome resamples the mic into a 16 kHz context. Echo cancellation lets hands-free mode
         * listen while the speaker plays; push-to-talk doesn't need it but it does no harm. */
        this.ctx = new AudioContext({ sampleRate: RATE, latencyHint: 'interactive' });
        const stream = await navigator.mediaDevices.getUserMedia({
            audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true },
        });
        await this.ctx.audioWorklet.addModule(new URL('./mic-worklet.js', import.meta.url));
        this.node = new AudioWorkletNode(this.ctx, 'mic-capture');
        this.node.port.onmessage = (e) => this.onChunk && this.onChunk(new Int16Array(e.data.pcm), e.data.peak);
        this.ctx.createMediaStreamSource(stream).connect(this.node);
        /* A worklet only runs while connected to the output; it writes nothing to it. */
        this.node.connect(this.ctx.destination);
    }

    mic(on) {
        this.node && this.node.port.postMessage(on ? 'on' : 'off');
    }

    /* Queues reply speech (16-bit PCM bytes) right after what's already queued. */
    play(bytes) {
        if (!this.ctx || bytes.byteLength < 2) {
            return;
        }
        const pcm = new Int16Array(bytes.buffer, bytes.byteOffset, bytes.byteLength >> 1);
        const buf = this.ctx.createBuffer(1, pcm.length, RATE);
        const out = buf.getChannelData(0);
        for (let i = 0; i < pcm.length; i++) {
            out[i] = pcm[i] / 0x8000;
        }
        const src = this.ctx.createBufferSource();
        src.buffer = buf;
        src.connect(this.ctx.destination);
        /* A little slack after a gap, so a late chunk doesn't start mid-click. */
        const at = Math.max(this.nextAt, this.ctx.currentTime + 0.03);
        src.start(at);
        this.nextAt = at + buf.duration;
        this.played += pcm.length;
        this.playing.add(src);
        src.onended = () => {
            this.playing.delete(src);
            if (!this.playing.size && this.onIdle) {
                this.onIdle();
            }
        };
    }

    get speaking() {
        return this.playing.size > 0;
    }

    /* Barge-in: drops everything queued. */
    stop() {
        for (const src of this.playing) {
            src.onended = null;
            try {
                src.stop();
            } catch {
                /* already ended */
            }
        }
        this.playing.clear();
        this.nextAt = 0;
        this.played = 0;
    }
}

export function b64encode(int16) {
    const bytes = new Uint8Array(int16.buffer, int16.byteOffset, int16.byteLength);
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) {
        s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return btoa(s);
}

export function b64decode(b64) {
    const s = atob(b64);
    const bytes = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) {
        bytes[i] = s.charCodeAt(i);
    }
    return bytes;
}
