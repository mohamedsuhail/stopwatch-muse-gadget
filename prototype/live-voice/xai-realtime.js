/*
 * xAI's Voice Agent API (wss://api.x.ai/v1/realtime), OpenAI-Realtime style
 * JSON events. The browser can't set an Authorization header on a WebSocket,
 * so it passes the server's short-lived token as the subprotocol
 * "xai-client-secret.<token>".
 */
export class XaiRealtime {
    constructor() {
        this.ws = null;
        this.onEvent = () => {};
        this.onClose = () => {};
    }

    async connect() {
        const r = await fetch('/api/session', { method: 'POST' });
        const s = await r.json();
        if (!r.ok) {
            throw new Error(s.error || `token request failed: HTTP ${r.status}`);
        }
        await new Promise((resolve, reject) => {
            const ws = new WebSocket(s.ws_url, ['xai-client-secret.' + s.token]);
            ws.onopen = () => {
                this.ws = ws;
                resolve();
            };
            ws.onerror = () => reject(new Error('WebSocket to xAI failed (see the browser console)'));
            ws.onmessage = (e) => {
                if (typeof e.data !== 'string') {
                    return;   /* binary transport isn't asked for */
                }
                let ev;
                try {
                    ev = JSON.parse(e.data);
                } catch {
                    return;
                }
                this.onEvent(ev);
            };
            ws.onclose = (e) => {
                const was = this.ws === ws;
                this.ws = null;
                if (was) {
                    this.onClose(e);
                } else {
                    reject(new Error(`xAI closed the connection: ${e.code} ${e.reason}`));
                }
            };
        });
    }

    get open() {
        return !!this.ws && this.ws.readyState === WebSocket.OPEN;
    }

    send(ev) {
        if (this.open) {
            this.ws.send(JSON.stringify(ev));
            return true;
        }
        return false;
    }

    close() {
        const ws = this.ws;
        this.ws = null;
        ws && ws.close();
    }
}
