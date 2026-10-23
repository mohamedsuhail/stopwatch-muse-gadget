/*
 * The page: wires the talk button, the avatar and the panels to the
 * Orchestrator (orchestrator.js), which does the actual conversation.
 */
import { Audio } from './audio.js';
import { MuseMock, DEFAULT_NOTES } from './muse-mock.js';
import { MuseSerial } from './muse-serial.js';
import { DEFAULT_STYLE, Orchestrator } from './orchestrator.js';
import { XaiRealtime } from './xai-realtime.js';

const $ = (id) => document.getElementById(id);
const KEY = 'mobot-live-voice';

/* ---- settings, kept in localStorage ---- */

const DEFAULTS = {
    museKind: 'mock', mirror: true, wait: 'block', speak: 'verbatim', routing: 'personal',
    voice: 'iris', handsFree: false, style: DEFAULT_STYLE, notes: DEFAULT_NOTES,
};
let settings = { ...DEFAULTS, ...JSON.parse(localStorage.getItem(KEY) || '{}') };
const SESSION_KEYS = ['wait', 'speak', 'routing', 'voice', 'handsFree'];

function showSettings() {
    for (const name of ['museKind', 'wait', 'speak', 'routing']) {
        for (const el of document.querySelectorAll(`input[name="${name}"]`)) {
            el.checked = el.value === settings[name];
        }
    }
    $('mirror').checked = settings.mirror;
    $('handsFree').checked = settings.handsFree;
    $('style').value = settings.style;
    $('notes').value = settings.notes;
    if (![...$('voice').options].some((o) => o.value === settings.voice)) {
        $('voice').add(new Option(settings.voice, settings.voice));
    }
    $('voice').value = settings.voice;
}

function setSetting(name, value) {
    settings[name] = value;
    localStorage.setItem(KEY, JSON.stringify(settings));
    if (SESSION_KEYS.includes(name) && rt.open) {
        orch.configure();
        sys(`Session updated (${name}: ${value})`);
    }
    if (name === 'museKind') {
        useMuse();
    }
}

/* ---- the avatar: the watch's clips, cross-faded like the firmware does ---- */

const CLIPS = '/avatar/character/clips/';
const LOOPS = new Set(['idle', 'listening', 'thinking', 'speaking']);
const STATE_CLIP = { off: 'idle', idle: 'idle', listening: 'listening', thinking: 'thinking', muse: 'thinking', speaking: 'speaking' };
const MOOD_CLIP = { joy: 'happy', thinking: 'thinking', sad: 'error', goodnight: 'goodbye' };
const MOOD_FACE = { joy: 'happy', thinking: 'thinking', sad: 'error', goodnight: 'idle' };
const STATE_WORDS = { off: 'Off', idle: 'Ready', listening: 'Listening', thinking: 'Thinking', muse: 'Asking Muse…', speaking: 'Speaking' };

let front = $('clipA');
let back = $('clipB');
let clip = '';
let moodUntil = 0;
let baseClip = 'idle';

function playClip(name) {
    if (name === clip) {
        return;
    }
    clip = name;
    back.src = CLIPS + name + '.mp4';
    back.loop = LOOPS.has(name);
    back.currentTime = 0;
    back.play().catch(() => {});
    back.onplaying = () => {
        back.onplaying = null;
        back.classList.remove('back');
        front.classList.add('back');
        [front, back] = [back, front];
        setTimeout(() => back.pause(), 150);
    };
}

function showState(st, asks) {
    $('screen').dataset.state = st;
    if (st === 'listening') {
        $('caption').textContent = '';
    }
    $('stateLabel').textContent = STATE_WORDS[st] + (asks > 1 ? ` (${asks})` : '');
    baseClip = STATE_CLIP[st];
    if (Date.now() > moodUntil) {
        playClip(baseClip);
    }
    $('talk').disabled = st === 'off';
}

function showMood(mood) {
    const name = MOOD_CLIP[mood];
    if (!name) {
        return;
    }
    sys(`set_mood(${mood})`);
    moodUntil = Date.now() + 3500;
    clip = '';
    playClip(name);
    orch.face(MOOD_FACE[mood]);
    setTimeout(() => {
        if (Date.now() >= moodUntil) {
            playClip(baseClip);
        }
    }, 3600);
}

/* ---- the conversation panel ---- */

const log = $('log');
let partial = null;
const cards = new Map();

function scroll() {
    log.parentElement.scrollTop = log.parentElement.scrollHeight;
}

function bubble(cls, who, text) {
    const el = document.createElement('div');
    el.className = 'msg ' + cls;
    if (who) {
        const w = document.createElement('span');
        w.className = 'who';
        w.textContent = who;
        el.append(w);
    }
    el.append(document.createTextNode(text));
    log.append(el);
    scroll();
    return el;
}

function sys(text) {
    bubble('sys', '', text);
}

function transcript(who, text, isPartial) {
    if (who === 'you') {
        if (!partial) {
            partial = bubble('you partial', 'You', '');
        }
        partial.lastChild.textContent = text;
        if (!isPartial) {
            partial.classList.remove('partial');
            partial = null;
        }
        scroll();
    } else {
        bubble('mobot', 'MOBOT', text);
    }
}

function museCard(e) {
    let c = cards.get(e.id);
    if (!c) {
        const el = document.createElement('div');
        el.className = 'card';
        el.innerHTML = '<div class="head"><span>ask_muse → ' + muse.name + '</span><span class="t"></span></div>' +
            '<div class="q"></div><div class="a"></div><div class="steps"></div>';
        el.querySelector('.q').textContent = '“' + e.question + '”';
        log.append(el);
        c = { el, steps: [] };
        cards.set(e.id, c);
    }
    const step = (s) => {
        c.steps.push(s);
        c.el.querySelector('.steps').textContent = c.steps.join(' · ');
    };
    const secs = (ms) => (ms / 1000).toFixed(1) + ' s';
    if (e.phase === 'asked') step('asked');
    if (e.phase === 'sent') step('Muse has it ' + secs(e.ms));
    if (e.phase === 'text') {
        if (!c.firstText) {
            c.firstText = true;
            step('first words ' + secs(e.ms));
        }
        c.el.querySelector('.a').textContent += e.text;
    }
    if (e.phase === 'answer') {
        c.el.querySelector('.a').textContent = e.answer;
        c.el.querySelector('.t').textContent = secs(e.ms);
        step('answered ' + secs(e.ms) + ' → ' + (settings.speak === 'verbatim' ? 'said word for word' : 'Grok retells it'));
    }
    if (e.phase === 'error') {
        step('failed: ' + e.error);
        c.el.style.borderColor = 'var(--bad)';
    }
    scroll();
}

/* ---- timings ---- */

const rows = new WeakMap();
let turns = 0;
const COLS = { audio: 1, tool: 2, muse: 3, answer: 4 };

function mark(key, label, ms, turn) {
    if (!turn) {
        sys(label);
        return;
    }
    let row = rows.get(turn);
    if (!row) {
        row = $('times').insertRow(0);
        row.insertCell().textContent = ++turns;
        for (let i = 0; i < 4; i++) row.insertCell().textContent = '–';
        rows.set(turn, row);
    }
    if (COLS[key]) {
        row.cells[COLS[key]].textContent = (ms / 1000).toFixed(2) + ' s';
    }
}

/* ---- raw events ---- */

const events = [];
function event(ev, notable) {
    if (!notable && !$('allEvents').checked) {
        return;
    }
    const brief = JSON.stringify(ev, (k, v) => (typeof v === 'string' && v.length > 200 ? v.slice(0, 200) + '…' : v));
    events.push(new Date().toISOString().slice(11, 23) + '  ' + brief);
    if (events.length > 400) events.shift();
    $('events').textContent = events.join('\n');
}

/* ---- wiring ---- */

const audio = new Audio();
const rt = new XaiRealtime();
const mock = new MuseMock(() => settings.notes);
let board = null;
let muse = mock;

const orch = new Orchestrator({
    rt, audio, muse, settings: () => settings,
    on: {
        state: showState,
        caption: (t) => { $('caption').textContent = t.length > 90 ? '…' + t.slice(-90).replace(/^\S*\s/, '') : t; },
        transcript,
        muse: museCard,
        mark,
        event,
        mood: showMood,
        error: (t) => bubble('err', '', t),
        level: (p) => { $('level').style.width = Math.min(100, p * 140) + '%'; },
    },
});

function useMuse() {
    const want = settings.museKind === 'serial' ? board : mock;
    if (!want) {
        $('musePill').textContent = 'Muse · watch not connected';
        $('musePill').className = 'pill bad';
        muse = mock;
    } else {
        muse = want;
        $('musePill').textContent = muse === mock ? 'Muse · mock' : 'Muse · watch';
        $('musePill').className = muse === mock ? 'pill' : 'pill ok';
    }
    if (orch.muse !== muse) {
        orch.abortAll();
        orch.muse = muse;
    }
}

async function connect() {
    if (rt.open) {
        rt.close();
        return;
    }
    $('connect').disabled = true;
    $('xaiPill').textContent = 'xAI · connecting';
    try {
        await audio.start();
        await rt.connect();
        $('xaiPill').textContent = 'xAI · live';
        $('xaiPill').className = 'pill ok';
        $('connect').textContent = 'Disconnect';
        $('caption').textContent = '';
        clip = '';
        playClip('wake');
        moodUntil = Date.now() + 3000;
        setTimeout(() => playClip(baseClip), 3000);
        orch.configure();
        sys('Connected to xAI. Hold the button (or Space) and talk.');
    } catch (e) {
        $('xaiPill').textContent = 'xAI · failed';
        $('xaiPill').className = 'pill bad';
        bubble('err', '', e.message);
    } finally {
        $('connect').disabled = false;
    }
}

rt.onClose = (e) => {
    $('xaiPill').textContent = 'xAI · closed';
    $('xaiPill').className = 'pill bad';
    $('connect').textContent = 'Connect';
    audio.stop();
    audio.mic(false);
    orch.abortAll();
    orch.talking = false;
    orch.response = null;
    orch.awaiting = false;
    orch.refresh();
    sys(`xAI closed the session (${e.code}${e.reason ? ': ' + e.reason : ''})`);
};

async function connectWatch() {
    try {
        board && (await board.close());
        board = null;
        $('musePill').textContent = 'Muse · opening port';
        board = await MuseSerial.connect((tag, text) => sys(`[${tag}] ${text}`));
        board.onRestart = () => sys('The watch restarted; typed turns resume once it has rejoined Wi-Fi.');
        settings.museKind = 'serial';
        showSettings();
        setSetting('museKind', 'serial');
        sys('Watch connected: questions for Muse go through it.');
    } catch (e) {
        bubble('err', '', 'Watch: ' + e.message);
        useMuse();
    }
}

$('connect').onclick = connect;
$('serialConnect').onclick = connectWatch;

const talk = $('talk');
talk.addEventListener('pointerdown', (e) => {
    talk.setPointerCapture(e.pointerId);
    talk.classList.add('down');
    orch.press();
});
for (const t of ['pointerup', 'pointercancel']) {
    talk.addEventListener(t, () => {
        talk.classList.remove('down');
        orch.release();
    });
}
const typing = () => ['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName);
window.addEventListener('keydown', (e) => {
    if (e.code === 'Space' && !e.repeat && !typing() && !talk.disabled) {
        e.preventDefault();
        talk.classList.add('down');
        orch.press();
    }
});
window.addEventListener('keyup', (e) => {
    if (e.code === 'Space' && !typing()) {
        e.preventDefault();
        talk.classList.remove('down');
        orch.release();
    }
});

for (const name of ['museKind', 'wait', 'speak', 'routing']) {
    for (const el of document.querySelectorAll(`input[name="${name}"]`)) {
        el.onchange = () => el.checked && setSetting(name, el.value);
    }
}
$('mirror').onchange = (e) => setSetting('mirror', e.target.checked);
$('handsFree').onchange = (e) => setSetting('handsFree', e.target.checked);
$('voice').onchange = (e) => setSetting('voice', e.target.value);
$('style').onchange = (e) => setSetting('style', e.target.value);
$('notes').onchange = (e) => setSetting('notes', e.target.value);
$('reset').onclick = () => {
    settings = { ...DEFAULTS };
    localStorage.removeItem(KEY);
    showSettings();
    useMuse();
    rt.open && orch.configure();
};

for (const b of document.querySelectorAll('nav button')) {
    b.onclick = () => {
        document.querySelectorAll('nav button, .tab').forEach((x) => x.classList.remove('on'));
        b.classList.add('on');
        $('tab-' + b.dataset.tab).classList.add('on');
    };
}

/* xAI's voices, if the server can list them; iris (the watch's TTS voice) and eve otherwise. */
fetch('/api/voices').then((r) => (r.ok ? r.json() : null)).then((v) => {
    const list = Array.isArray(v) ? v : v?.voices || v?.data || [];
    const ids = list.map((x) => (typeof x === 'string' ? x : x.voice_id || x.id || x.name)).filter(Boolean);
    if (ids.length) {
        $('voice').innerHTML = '';
        for (const id of ids) $('voice').add(new Option(id, id));
        showSettings();
    }
}).catch(() => {});

showSettings();
useMuse();
playClip('idle');
window.mobot = { orch, settings: () => settings };   /* for poking at it from the console */
