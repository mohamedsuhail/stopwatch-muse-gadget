/*
 * Mic capture, in the audio thread. The context runs at 16 kHz (the watch's
 * mic rate), so this only converts float samples to 16-bit and posts them in
 * 40 ms chunks (640 frames), about as often as the watch's voice task feeds Hatch.
 */
const CHUNK = 640;

class MicCapture extends AudioWorkletProcessor {
    constructor() {
        super();
        this.buf = new Int16Array(CHUNK);
        this.n = 0;
        this.on = false;
        this.port.onmessage = (e) => {
            this.on = e.data === 'on';
            this.n = 0;
        };
    }

    process(inputs) {
        const ch = inputs[0] && inputs[0][0];
        if (!ch) {
            return true;
        }
        let peak = 0;
        for (let i = 0; i < ch.length; i++) {
            const s = Math.max(-1, Math.min(1, ch[i]));
            peak = Math.max(peak, Math.abs(s));
            if (this.on) {
                this.buf[this.n++] = s < 0 ? s * 0x8000 : s * 0x7fff;
                if (this.n === CHUNK) {
                    this.port.postMessage({ pcm: this.buf.buffer, peak }, [this.buf.buffer]);
                    this.buf = new Int16Array(CHUNK);
                    this.n = 0;
                    peak = 0;
                }
            }
        }
        return true;
    }
}

registerProcessor('mic-capture', MicCapture);
