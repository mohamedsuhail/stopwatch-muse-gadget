import base64, json, os, struct, sys, urllib.request

PHRASES = [
    "Hmm, let me think.",
    "Good question!",
    "One moment.",
    "Let me see.",
    "Ooh, interesting.",
    "Just a sec.",
    "Okay, thinking.",
    "Hmm, good one.",
    "Let me check.",
    "Right, one second.",
]
KEY = open(os.path.expanduser("~/.config/stopwatch/gemini_key")).read().strip()
URL = "https://generativelanguage.googleapis.com/v1beta/interactions?alt=sse"
out_dir = sys.argv[1]

def tts(text):
    body = json.dumps({
        "model": "gemini-3.8-flash-lite-tts",
        "input": [{"type": "user_input", "content": [{"type": "text", "text": text}]}],
        "response_format": {"type": "audio", "mime_type": "audio/l16"},
        "generation_config": {"speech_config": [{"voice": "Achernar"}]},
        "stream": True,
    }).encode()
    req = urllib.request.Request(URL, body, {"Content-Type": "application/json", "x-goog-api-key": KEY})
    pcm, rate = b"", 24000
    with urllib.request.urlopen(req, timeout=60) as r:
        for line in r:
            line = line.decode().strip()
            if line.startswith("data:") and "[DONE]" not in line:
                d = json.loads(line[5:]).get("delta", {})
                if d.get("type") == "audio":
                    pcm += base64.b64decode(d["data"])
                    rate = d.get("sample_rate", rate)
    return struct.unpack("<%dh" % (len(pcm) // 2), pcm[: len(pcm) // 2 * 2]), rate

def to16k(s, rate):
    if rate == 16000:
        return list(s)
    out, step, pos = [], rate / 16000.0, 0.0
    while pos < len(s) - 1:
        i = int(pos); f = pos - i
        out.append(int(s[i] + (s[i + 1] - s[i]) * f))
        pos += step
    return out

def trim(s, thresh=300, pad=800):
    loud = [i for i, v in enumerate(s) if abs(v) > thresh]
    if not loud:
        return s
    return s[max(0, loud[0] - pad): loud[-1] + pad]

clips = []
for p in PHRASES:
    s, rate = tts(p)
    s = trim(to16k(s, rate))
    peak = max(abs(v) for v in s) or 1
    g = min(1.0, 26000 / peak)                 # same loudness as the replies, no clipping
    s = [max(-32768, min(32767, int(v * g))) for v in s]
    clips.append(struct.pack("<%dh" % len(s), *s))
    print("%-22s %.2fs" % (p, len(s) / 16000))

# fillers.pcm: u32 count, then (u32 offset, u32 bytes) per clip, then 16 kHz mono s16le PCM.
hdr = 4 + 8 * len(clips)
blob, index, off = b"", b"", hdr
for c in clips:
    index += struct.pack("<II", off, len(c)); blob += c; off += len(c)
open(os.path.join(out_dir, "fillers.pcm"), "wb").write(struct.pack("<I", len(clips)) + index + blob)
open(os.path.join(out_dir, "fillers.txt"), "w").write("\n".join(PHRASES) + "\n")
print("total %.0f KB" % ((hdr + len(blob)) / 1024))
