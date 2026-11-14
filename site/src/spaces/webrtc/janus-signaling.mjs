// Janus carries signaling only. SDP belongs in the plugin body, never in JSEP.
// Credentials and password-derived room IDs remain in memory for this session.
export class JanusSignaling {
  constructor(room, endpoint) {
    if (!/^[A-F0-9]{32}$/.test(room)) throw Error('Invalid private room identifier');
    const url = new URL(endpoint);
    if (url.username || url.password || url.search || url.hash ||
        (url.protocol !== 'wss:' && !(url.protocol === 'ws:' && ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)))) {
      throw Error('Use a secure wss:// signaling URL without credentials or query parameters.');
    }
    this.room = room;
    this.pending = new Map();
    this.closed = false;
    this.socket = new WebSocket(url.href, 'janus-protocol');
    this.socket.onopen = () => { if (!this.closed) this.request('create', {}, 'create'); };
    this.socket.onmessage = event => {
      if (this.closed) return;
      try {
        if (typeof event.data !== 'string' || event.data.length > 65536) throw Error('Invalid signaling response');
        const response = JSON.parse(event.data);
        if (response.janus === 'error') throw Error('The signaling server rejected this connection.');
        if (['timeout', 'detached'].includes(response.janus)) throw Error('Signaling session ended. Reconnect to the camera.');
        const operation = this.pending.get(response.transaction);
        if (response.janus === 'success' && operation === 'create') {
          this.pending.delete(response.transaction);
          if (!Number.isSafeInteger(response.data?.id) || response.data.id < 1) throw Error('Invalid Janus session');
          this.session = response.data.id;
          this.heartbeat = setInterval(() => {
            if (this.socket.readyState === WebSocket.OPEN) this.request('keepalive');
          }, 20000);
          this.request('attach', { plugin: 'janus.plugin.signaling' }, 'attach');
        } else if (response.janus === 'success' && operation === 'attach') {
          this.pending.delete(response.transaction);
          if (!Number.isSafeInteger(response.data?.id) || response.data.id < 1) throw Error('Invalid Janus handle');
          this.handle = response.data.id;
          this.request('message', { body: { request: 'join', room: this.room, role: 'viewer' } }, 'join');
        }
        const data = response.plugindata?.data;
        if (data?.error) throw Error(data.error);
        if (data?.event === 'joined') { this.pending.delete(response.transaction); this.joined = true; }
        if (data?.event !== 'signal') return;
        if (response.sender !== this.handle) throw Error('Unexpected Janus signaling handle');
        let message = data.message;
        if (message?.type === 'customized') {
          const errors = { busy: 'Camera is already in use.', password_not_configured: 'Set the camera password during setup first.', authentication_rate_limited: 'Too many login attempts; wait 30 seconds.' };
          if (errors[message.data]) throw Error(errors[message.data]);
          if (typeof message.data !== 'string' || !message.data.startsWith('{')) return;
          message = JSON.parse(message.data);
          if (message.type !== 'ready') return;
        }
        if (['ready', 'answer', 'candidate', 'bye'].includes(message?.type)) this.onmessage?.({ data: JSON.stringify(message) });
      } catch (error) { this.onerror?.(error); }
    };
    this.socket.onerror = () => { if (!this.closed) this.onerror?.(Error('Cannot reach the Janus signaling server.')); };
    this.socket.onclose = () => { clearInterval(this.heartbeat); if (!this.closed) this.onclose?.(); };
  }
  request(janus, fields = {}, operation) {
    if (this.closed || this.socket.readyState !== WebSocket.OPEN) throw Error('Signaling connection is closed');
    const transaction = crypto.randomUUID();
    if (operation) this.pending.set(transaction, operation);
    const message = { janus, transaction, ...fields };
    if (this.session) message.session_id = this.session;
    if (this.handle && janus === 'message') message.handle_id = this.handle;
    this.socket.send(JSON.stringify(message));
  }
  send(data) {
    if (!this.handle) throw Error('Signaling session is not ready');
    this.request('message', { body: { request: 'send', message: JSON.parse(data) } });
  }
  close() {
    if (this.closed) return;
    clearInterval(this.heartbeat);
    if (this.socket.readyState === WebSocket.OPEN && this.session) this.request('destroy');
    this.closed = true;
    this.socket.close();
    this.room = null; this.pending.clear();
  }
}
