/*
 * A stand-in Muse for trying the orchestration without the watch: it "knows"
 * the notes in the page's Mock Muse box, takes 2 to 6 s like the real one, and
 * streams its reply in pieces. It answers with the note that shares the most
 * words with the question, so it's only good for checking the plumbing.
 */
const STOP = new Set(('a an the is are was were be to of in on for and or my me i you your it its this that what ' +
    "whats what's how do does did with about tell can could would should please muse ask").split(' '));

function words(s) {
    return s.toLowerCase().match(/[a-z0-9']+/g)?.filter((w) => w.length > 1 && !STOP.has(w)) || [];
}

export class MuseMock {
    constructor(notes, { minMs = 2000, maxMs = 6000 } = {}) {
        this.name = 'Mock Muse';
        this.notes = notes;
        this.minMs = minMs;
        this.maxMs = maxMs;
    }

    answer(question) {
        const q = new Set(words(question.replace(/^\(.*?\)\s*/s, '')));   /* not the style note */
        let best = null;
        let score = 0;
        for (const note of this.notes().split('\n').map((l) => l.trim()).filter(Boolean)) {
            const s = words(note).filter((w) => q.has(w)).length;
            if (s > score) {
                best = note;
                score = s;
            }
        }
        return best || "I don't have anything about that in your notes yet.";
    }

    async ask(text, progress = () => {}, signal) {
        const wait = (ms) => new Promise((resolve, reject) => {
            const t = setTimeout(resolve, ms);
            signal && signal.addEventListener('abort', () => {
                clearTimeout(t);
                reject(new Error('cancelled'));
            });
        });
        await wait(150);
        progress({ type: 'sent' });
        await wait(this.minMs + Math.random() * (this.maxMs - this.minMs));
        const reply = this.answer(text);
        const pieces = reply.match(/.{1,40}(\s|$)/g) || [reply];
        for (const p of pieces) {
            progress({ type: 'text', msg: 0, text: p });
            await wait(60);
        }
        progress({ type: 'done', complete: true });
        return reply;
    }

    async face() {}

    async close() {}
}

export const DEFAULT_NOTES = `The StopWatch Muse gadget is an M5Stack StopWatch running Muse with a toy astronaut avatar called MOBOT; it arrived on October 21 and was talking by October 22.
Spoken replies on the StopWatch use xAI text to speech with the voice iris, and Gemini TTS is the fallback.
The next StopWatch task is moving the SDK checkout out of the iCloud-synced Documents folder, because iCloud broke several builds.
Internal RAM on the StopWatch bottoms out near 18K free after four fixes; Wi-Fi and TLS fail below about 5K.
The Instagram video about the StopWatch is planned as hands-only with a voiceover, starting with a "my stopwatch talks back" reel.`;
