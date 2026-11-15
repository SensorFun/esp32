// The signaling server carries opaque application data only, never JSEP.
// WebRTC stays end-to-end between camera and viewer, including its data channel.
export class Signaling {
  constructor(room, endpoint) {
    if (!/^[A-F0-9]{32}$/.test(room)) throw Error('Invalid private room identifier');
    const url = new URL(endpoint);
    if (url.username || url.password || url.search || url.hash ||
        (url.protocol !== 'wss:' && !(url.protocol === 'ws:' && ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)))) {
      throw Error('Use a secure wss:// signaling URL without credentials or query parameters.');
    }
    this.room = room;
    this.joined = false;
    this.closed = false;
    this.socket = new WebSocket(url.href);
    this.socket.onopen = () => {
      if (!this.closed) this.socket.send(JSON.stringify({ type: 'join', room: this.room }));
    };
    this.socket.onmessage = event => {
      if (this.closed) return;
      try {
        if (typeof event.data !== 'string' || event.data.length > 65536) throw Error('Invalid signaling response');
        let message = JSON.parse(event.data);
        if (message.type === 'error') throw Error(message.message || 'The signaling server rejected this connection.');
        if (message.type === 'joined') { this.joined = true; return; }
        if (message.type === 'customized') {
          const errors = { busy: 'Camera is already in use.', password_not_configured: 'Set the camera password during setup first.', authentication_rate_limited: 'Too many login attempts; wait 30 seconds.' };
          if (errors[message.data]) throw Error(errors[message.data]);
          if (typeof message.data !== 'string' || !message.data.startsWith('{')) return;
          message = JSON.parse(message.data);
          if (message.type !== 'ready') return;
        }
        if (['ready', 'answer', 'candidate', 'bye'].includes(message?.type)) this.onmessage?.({ data: JSON.stringify(message) });
      } catch (error) { this.onerror?.(error); }
    };
    this.socket.onerror = () => { if (!this.closed) this.onerror?.(Error('Cannot reach the signaling server.')); };
    this.socket.onclose = () => { if (!this.closed) this.onclose?.(); };
  }
  send(data) {
    if (!this.joined) throw Error('Signaling session is not ready');
    this.socket.send(data);
  }
  close() {
    if (this.closed) return;
    this.closed = true;
    this.socket.close();
    this.room = null;
  }
}
