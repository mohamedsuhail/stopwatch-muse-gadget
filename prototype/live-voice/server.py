#!/usr/bin/env python3
"""The live-voice prototype's local server. Standard library only.

    XAI_API_KEY=... python3 prototype/live-voice/server.py [--port 8765]

Then open http://localhost:8765/ in Chrome or Edge (Web Serial, for the watch).

It serves the repo root, so the page can play the astronaut clips in
avatar/character/clips/, and it keeps the xAI key out of the browser:

    POST /api/session   mints a short-lived client secret (POST /v1/realtime/client_secrets)
                        and returns {"token", "ws_url"}; the page passes the token to xAI as
                        the WebSocket subprotocol "xai-client-secret.<token>"
    GET  /api/voices    xAI's voice list (GET /v1/tts/voices), for the voice picker

XAI_API_BASE and XAI_WS_URL point it at another server (the tests' fake xAI).
"""
import argparse
import http.server
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]   # the repo
PAGE = "/prototype/live-voice/"
API_BASE = os.environ.get("XAI_API_BASE", "https://api.x.ai").rstrip("/")
WS_URL = os.environ.get("XAI_WS_URL", "wss://api.x.ai/v1/realtime")
MODEL = os.environ.get("XAI_VOICE_MODEL", "grok-voice-latest")
TOKEN_SECONDS = 600


def xai(method, path, body=None):
    key = os.environ.get("XAI_API_KEY", "")
    if not key:
        raise RuntimeError("XAI_API_KEY isn't set in the server's environment")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API_BASE + path, data=data, method=method, headers={
        "Authorization": "Bearer " + key,
        "Content-Type": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"xAI {path}: HTTP {e.code} {e.read()[:300].decode('utf-8', 'replace')}")


def token_of(reply):
    """The secret in a client_secrets reply, wherever this API version puts it."""
    for k in ("value", "token", "client_secret", "secret"):
        v = reply.get(k)
        if isinstance(v, str) and v:
            return v
        if isinstance(v, dict) and isinstance(v.get("value"), str):
            return v["value"]
    raise RuntimeError("no token in the client_secrets reply: keys " + ", ".join(reply))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT), **kw)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")   # a prototype: always the latest files
        super().end_headers()

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(302)
            self.send_header("Location", PAGE)
            self.end_headers()
        elif self.path == "/api/voices":
            try:
                self.send_json(200, xai("GET", "/v1/tts/voices"))
            except RuntimeError as e:
                self.send_json(502, {"error": str(e)})
        else:
            super().do_GET()

    def do_POST(self):
        if self.path != "/api/session":
            self.send_error(404)
            return
        try:
            reply = xai("POST", "/v1/realtime/client_secrets", {"expires_after": {"seconds": TOKEN_SECONDS}})
            self.send_json(200, {"token": token_of(reply), "ws_url": WS_URL + "?model=" + MODEL})
        except RuntimeError as e:
            self.log_message("session: %s", e)
            self.send_json(502, {"error": str(e)})

    def log_message(self, fmt, *args):
        line = fmt % args
        if "/api/" in line or not fmt.startswith('"%s"'):   # API calls and errors; not every file
            sys.stderr.write("[server] " + line + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    if not os.environ.get("XAI_API_KEY"):
        print("warning: XAI_API_KEY isn't set; the page can't start a voice session", file=sys.stderr)
    # localhost only: Chrome treats it as a secure context (mic, Web Serial), and nothing else can reach it
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Live-voice prototype: http://localhost:{args.port}/")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
