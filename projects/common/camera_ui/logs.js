// Shared by the firmware's local page and the remote React viewer.
class CameraLogs extends HTMLElement {
  constructor() {
    super(); this.attachShadow({ mode: 'open' }); this.cursor = 0; this.lines = [];
    this.shadowRoot.innerHTML = `<style>
      :host{display:block;min-width:0;color:#e4eadd;font:16px system-ui}:host([hidden]){display:none}
      *{box-sizing:border-box}section{padding:18px;background:#17201a;border:1px solid #2a352c;border-radius:8px;min-width:0}
      h2{font-size:20px;margin:0 0 12px}button{font:inherit;min-height:44px;padding:10px 16px;background:#00d4cb;color:#07302c;border:0;border-radius:6px;cursor:pointer}button:disabled{opacity:.5;cursor:default}.actions{display:flex;flex-wrap:wrap;gap:12px}button.clear{background:transparent;color:#d8e8cb;border:1px solid #4a5a4c}
      pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:360px;overflow:auto;font:13px/1.6 monospace;min-width:0}
      p{line-height:1.5;color:#b7c9ab}button:focus-visible{outline:2px solid white;outline-offset:3px}
    </style><section><h2>Diagnostics</h2><p>Recent main events from this boot. Updates while connected.</p><div class="actions"><button type="button">Download logs</button><button type="button" class="core" disabled>Download crash summary</button><button type="button" class="clear">Clear logs</button></div><p class="core-status" role="status"></p><p role="status" class="connection-status">Connect a camera to view logs.</p><pre aria-label="Camera diagnostic events"></pre></section>`;
    this.shadowRoot.querySelector('button').onclick = () => {
      const report = ['Camera diagnostics', `Exported ${new Date().toISOString()}`, ...this.lines].join('\n');
      const url = URL.createObjectURL(new Blob([report], { type: 'text/plain' }));
      const link = document.createElement('a'); link.href = url; link.download = 'camera-diagnostics.txt'; link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    };
    this.shadowRoot.querySelector('.core').onclick = () => this.downloadCore();
    this.shadowRoot.querySelector('.clear').onclick = () => this.clear();
  }
  // Clears this page and, when connected, the camera's event history too.
  clear() {
    this.lines = [];
    this.shadowRoot.querySelector('pre').textContent = '';
    if (this.online) {
      // Log replies already in flight predate the clear; drop them until the
      // camera acknowledges it.
      this.clearing = true;
      this.send({ op: 'logs_clear' });
      this.recordBrowser('Logs cleared on this page and the camera');
    } else this.recordBrowser('Logs cleared on this page');
  }
  send(detail) {
    this.dispatchEvent(new CustomEvent('camera-command', { detail, bubbles: true, composed: true }));
  }
  coreStatus(text) { this.shadowRoot.querySelector('.core-status').textContent = text; }
  // The raw core dump holds task stacks with keys, passwords and addresses, so
  // the camera only sends a text summary: task, exception and code addresses.
  finishCore(message) {
    clearTimeout(this.coreTimer);
    this.corePending = false;
    this.shadowRoot.querySelector('.core').disabled = !this.online;
    this.coreStatus(message);
  }
  downloadCore() {
    if (this.corePending || !this.online) return;
    this.corePending = true;
    this.coreRequest = (this.coreRequest || 0) + 1;
    this.shadowRoot.querySelector('.core').disabled = true;
    this.coreStatus('Requesting crash summary…');
    clearTimeout(this.coreTimer);
    this.coreTimer = setTimeout(() => this.finishCore('Crash summary request timed out. Please retry.'), 15000);
    this.send({ op: 'crash_summary', request_id: this.coreRequest });
  }
  connectedCallback() { this.timer = setInterval(() => this.poll(), 2500); }
  disconnectedCallback() { clearInterval(this.timer); clearTimeout(this.coreTimer); }
  set connected(value) {
    if (!value && this.corePending) this.finishCore('Connection lost. Reconnect and retry the crash summary.');
    this.online = value;
    this.clearing = false;
    this.shadowRoot.querySelector('.core').disabled = !value;
    this.shadowRoot.querySelector('.connection-status').textContent = value ? 'Waiting for recent events…' : 'Disconnected. Displayed events are retained in this page.';
    if (value) { this.cursor = 0; this.recordBrowser("Camera connection authenticated"); this.poll(); }
  }
  poll() {
    if (!this.online || document.hidden) return;
    this.dispatchEvent(new CustomEvent('camera-command', { detail: { op: 'logs_get', after: this.cursor }, bubbles: true, composed: true }));
  }
  recordBrowser(message) {
    this.lines.push(`[Browser ${new Date().toISOString()}] ${message}`);
    this.lines = this.lines.slice(-1000);
    this.shadowRoot.querySelector('pre').textContent = this.lines.join('\n');
  }
  reset() { if (this.online) this.recordBrowser('Camera connection closed. Final device events may be unavailable.'); this.connected = false; }
  handle(data) {
    if (data.type === 'crash_summary') {
      if (!this.corePending || data.request_id !== this.coreRequest) return;
      if (data.error || typeof data.text !== 'string') { this.finishCore(data.error || 'Invalid crash summary response.'); return; }
      const url = URL.createObjectURL(new Blob([data.text], { type: 'text/plain' }));
      const link = document.createElement('a'); link.href = url; link.download = 'camera-crash-summary.txt';
      this.shadowRoot.append(link); link.click(); link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      this.finishCore('Crash summary downloaded. The saved crash remains on the camera.');
      this.recordBrowser('Crash summary downloaded');
      return;
    }
    if (data.type !== 'logs') return;
    if (this.clearing && !data.cleared) return;
    if (data.cleared) this.clearing = false;
    if (data.board) this.recordBrowser(`Board ${data.board}; firmware ${data.firmware}; IDF ${data.idf}; ELF ${data.elf_sha256}; reset reason ${data.reset_reason}`);
    if (data.gap) this.lines.push('Some older events expired from the camera buffer.');
    for (const entry of data.entries || []) {
      if (entry.id <= this.cursor) continue;
      this.cursor = entry.id;
      this.lines.push(`[${(entry.ms / 1000).toFixed(1)}s] ${entry.level}: ${entry.message}${entry.repeats > 1 ? ` (repeated ${entry.repeats} times)` : ''}`);
    }
    this.lines = this.lines.slice(-1000);
    const output = this.shadowRoot.querySelector('pre');
    const bottom = output.scrollHeight - output.scrollTop - output.clientHeight < 30;
    output.textContent = this.lines.join('\n');
    if (bottom) output.scrollTop = output.scrollHeight;
    this.shadowRoot.querySelector('.connection-status').textContent = 'Connected · times are since camera startup · device history clears on camera restart.';
  }
}
customElements.define('sensorfan-camera-logs', CameraLogs);
