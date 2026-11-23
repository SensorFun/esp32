// Shared by the firmware's local page and the remote React viewer.
class CameraLogs extends HTMLElement {
  constructor() {
    super(); this.attachShadow({ mode: 'open' }); this.cursor = 0; this.lines = [];
    this.shadowRoot.innerHTML = `<style>
      :host{display:block;min-width:0;color:#e4eadd;font:16px system-ui}:host([hidden]){display:none}
      *{box-sizing:border-box}section{padding:18px;background:#17201a;border:1px solid #2a352c;border-radius:8px;min-width:0}
      h2{font-size:20px;margin:0 0 12px}button{font:inherit;min-height:44px;padding:10px 16px;background:#00d4cb;color:#07302c;border:0;border-radius:6px;cursor:pointer}
      pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:360px;overflow:auto;font:13px/1.6 monospace;min-width:0}
      p{line-height:1.5;color:#b7c9ab}button:focus-visible{outline:2px solid white;outline-offset:3px}
    </style><section><h2>Diagnostics</h2><p>Recent main events from this boot. Updates while connected.</p><button type="button">Download logs</button><p role="status">Connect a camera to view logs.</p><pre aria-label="Camera diagnostic events"></pre></section>`;
    this.shadowRoot.querySelector('button').onclick = () => {
      const url = URL.createObjectURL(new Blob([this.lines.join('\n')], { type: 'text/plain' }));
      const link = document.createElement('a'); link.href = url; link.download = 'camera-diagnostics.txt'; link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    };
  }
  connectedCallback() { this.timer = setInterval(() => this.poll(), 2500); }
  disconnectedCallback() { clearInterval(this.timer); }
  set connected(value) {
    this.online = value;
    this.shadowRoot.querySelector('[role=status]').textContent = value ? 'Waiting for recent events…' : 'Disconnected. Displayed events are retained in this page.';
    if (value) { this.cursor = 0; this.recordBrowser("Camera connection authenticated"); this.poll(); }
  }
  poll() {
    if (!this.online || document.hidden) return;
    this.dispatchEvent(new CustomEvent('camera-command', { detail: { op: 'logs_get', after: this.cursor }, bubbles: true, composed: true }));
  }
  recordBrowser(message) {
    this.lines.push(`[Browser ${new Date().toISOString()}] ${message}`);
    this.lines = this.lines.slice(-200);
    this.shadowRoot.querySelector('pre').textContent = this.lines.join('\n');
  }
  reset() { if (this.online) this.recordBrowser('Camera connection closed. Final device events may be unavailable.'); this.connected = false; }
  handle(data) {
    if (data.type !== 'logs') return;
    if (data.gap) this.lines.push('Some older events expired from the camera buffer.');
    for (const entry of data.entries || []) {
      if (entry.id <= this.cursor) continue;
      this.cursor = entry.id;
      this.lines.push(`[${(entry.ms / 1000).toFixed(1)}s] ${entry.level}: ${entry.message}${entry.repeats > 1 ? ` (repeated ${entry.repeats} times)` : ''}`);
    }
    this.lines = this.lines.slice(-200);
    const output = this.shadowRoot.querySelector('pre');
    const bottom = output.scrollHeight - output.scrollTop - output.clientHeight < 30;
    output.textContent = this.lines.join('\n');
    if (bottom) output.scrollTop = output.scrollHeight;
    this.shadowRoot.querySelector('[role=status]').textContent = 'Connected · times are since camera startup · device history clears on camera restart.';
  }
}
customElements.define('sensorfan-camera-logs', CameraLogs);
