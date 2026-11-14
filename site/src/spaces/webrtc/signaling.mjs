// Espressif's public collider accepts browser WebSockets directly. Room IDs
// are capabilities: never put them in logs, UI, source configuration, or storage.
export const SIGNALING_URL = 'wss://webrtc.espressif.com:8089/ws';
export class EspressifSignaling {
  constructor(room) {
    if (!/^[A-F0-9]{32}$/.test(room)) throw Error('Invalid private room identifier');
    this.room = room;
    this.client = Array.from(crypto.getRandomValues(new Uint8Array(12)), value => value.toString(16).padStart(2, '0')).join('');
    this.closed = false;
    this.registered = false;
    this.socket = new WebSocket(SIGNALING_URL);
    this.socket.onopen = () => {
      if (this.closed) { this.socket.close(); return; }
      this.socket.send(JSON.stringify({ cmd: 'register', roomid: room, clientid: this.client }));
      this.registered = true;
      this.send(JSON.stringify({ type: 'customized', data: 'connect' }));
    };
    this.socket.onmessage = event => {
      if (this.closed) return;
      try {
        const envelope = JSON.parse(event.data);
        if (envelope.error) throw Error('Signaling service rejected the connection');
        let message = envelope.msg ? JSON.parse(envelope.msg) : envelope;
        if (message.type === 'customized') {
          if (message.data === 'busy') throw Error('Camera is already in use.');
          if (message.data === 'password_not_configured') throw Error('Set the camera password during setup first.');
          if (message.data === 'authentication_rate_limited') throw Error('Too many login attempts; wait 30 seconds.');
          if (typeof message.data !== 'string' || !message.data.startsWith('{')) return;
          message = JSON.parse(message.data);
          if (message.type !== 'ready') return;
        }
        if (['ready', 'answer', 'candidate', 'bye'].includes(message.type)) this.onmessage?.({ data: JSON.stringify(message) });
      } catch (error) { this.onerror?.(error); }
    };
    this.socket.onerror = () => { if (!this.closed) this.onerror?.(Error('Cannot reach Espressif signaling.')); };
    this.socket.onclose = () => { if (!this.closed) this.onclose?.(); };
  }
  send(data) {
    if (this.closed || this.socket.readyState !== WebSocket.OPEN) throw Error('Signaling connection is closed');
    this.socket.send(JSON.stringify({ cmd: 'send', msg: data }));
  }
  close() {
    if (this.closed) return;
    this.closed = true;
    if (this.socket.readyState === WebSocket.OPEN) this.socket.send(JSON.stringify({ cmd: 'send', msg: JSON.stringify({ type: 'bye' }) }));
    this.socket.close();
    if (this.registered) {
      // Explicitly release this viewer's collider entry; no AppRTC HTTP room join
      // is performed by the browser. This endpoint permits cross-origin DELETE.
      fetch(`https://webrtc.espressif.com:8089/${this.room}/${this.client}`, { method: 'DELETE', keepalive: true }).catch(() => {});
    }
    this.room = null;
  }
}
