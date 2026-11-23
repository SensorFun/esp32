// Shared by the firmware's local page and the remote React viewer.
class CameraLogs extends HTMLElement {
  constructor() {
    super(); this.attachShadow({ mode: 'open' }); this.cursor = 0; this.lines = [];
    this.shadowRoot.innerHTML = `<style>
      :host{display:block;min-width:0;color:#e4eadd;font:16px system-ui}:host([hidden]){display:none}
      *{box-sizing:border-box}section{padding:18px;background:#17201a;border:1px solid #2a352c;border-radius:8px;min-width:0}
      h2{font-size:20px;margin:0 0 12px}button{font:inherit;min-height:44px;padding:10px 16px;background:#00d4cb;color:#07302c;border:0;border-radius:6px;cursor:pointer}button:disabled{opacity:.5;cursor:default}.actions{display:flex;flex-wrap:wrap;gap:12px}
      pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:360px;overflow:auto;font:13px/1.6 monospace;min-width:0}
      p{line-height:1.5;color:#b7c9ab}button:focus-visible{outline:2px solid white;outline-offset:3px}
    </style><section><h2>Diagnostics</h2><p>Recent main events from this boot. Updates while connected.</p><div class="actions"><button type="button">Download logs</button><button type="button" class="core" disabled>Download core dump</button></div><p class="core-status" role="status"></p><p role="status" class="connection-status">Connect a camera to view logs.</p><pre aria-label="Camera diagnostic events"></pre></section>`;
    this.shadowRoot.querySelector('button').onclick = () => {
      const report = ['Camera diagnostics', `Exported ${new Date().toISOString()}`, ...this.lines].join('\n');
      const url = URL.createObjectURL(new Blob([report], { type: 'text/plain' }));
      const link = document.createElement('a'); link.href = url; link.download = 'camera-diagnostics.txt'; link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    };
    this.shadowRoot.querySelector('.core').onclick = () => this.downloadCore();
  }
  send(detail) {
    this.dispatchEvent(new CustomEvent('camera-command', { detail, bubbles: true, composed: true }));
  }
  coreStatus(text) { this.shadowRoot.querySelector('.core-status').textContent = text; }
  finishCore(message, cancel = false) {
    clearTimeout(this.coreTimer);
    this.coreTransfer = null;
    this.corePending = false;
    const button = this.shadowRoot.querySelector('.core');
    button.textContent = 'Download core dump'; button.disabled = !this.online;
    this.coreStatus(message);
    if (cancel && this.online) this.send({ op: 'core_dump_cancel' });
  }
  coreTimeout() {
    clearTimeout(this.coreTimer);
    this.coreTimer = setTimeout(() => this.finishCore('Core dump download timed out. Please retry.', true), 45000);
  }
  downloadCore() {
    if (this.corePending) { this.finishCore('Core dump download cancelled.', true); return; }
    if (!this.online) return;
    this.corePending = true;
    this.coreRequest = (this.coreRequest || 0) + 1;
    this.shadowRoot.querySelector('.core').textContent = 'Cancel core dump download';
    this.coreStatus('Requesting saved core dump…');
    this.coreTimeout(); this.send({ op: 'core_dump_download', request_id: this.coreRequest });
  }
  binary(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 16) return false;
    const view = new DataView(buffer);
    if (view.getUint32(0, true) !== 0x45524f43) return false;
    const transfer = this.coreTransfer;
    if (!transfer || view.getUint32(4, true) !== transfer.id) return true;
    const total = view.getUint32(8, true), offset = view.getUint32(12, true);
    const bytes = new Uint8Array(buffer, 16);
    if (total !== transfer.bytes.length || offset !== transfer.received || !bytes.length || offset + bytes.length > total) {
      this.finishCore('Invalid core dump chunk. Please retry.', true); return true;
    }
    transfer.bytes.set(bytes, offset); transfer.received += bytes.length;
    this.coreTimeout();
    this.coreStatus(`Downloading core dump · ${transfer.received} of ${total} bytes`);
    if (transfer.received === total) {
      const url = URL.createObjectURL(new Blob([transfer.bytes], { type: 'application/octet-stream' }));
      const link = document.createElement('a'); link.href = url; link.download = 'camera-core-dump.bin';
      this.shadowRoot.append(link); link.click(); link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      this.finishCore(`Core dump downloaded · ${total} bytes. The saved dump remains on the camera.`);
      this.recordBrowser(`Core dump downloaded (${total} bytes)`);
    }
    return true;
  }
  connectedCallback() { this.timer = setInterval(() => this.poll(), 2500); }
  disconnectedCallback() { clearInterval(this.timer); this.finishCore('Core dump download interrupted.', true); }
  set connected(value) {
    if (!value && this.corePending) this.finishCore('Connection lost. Reconnect and retry the core dump download.', true);
    this.online = value;
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
    if (data.type === 'core_dump') {
      if (!this.corePending || data.request_id !== this.coreRequest) return;
      if (data.error) { this.finishCore(data.error); return; }
      if (!Number.isInteger(data.id) || !Number.isInteger(data.size) || data.size < 1 || data.size > 256 * 1024) {
        this.finishCore('Invalid core dump response.', true); return;
      }
      this.coreTransfer = { id: data.id, bytes: new Uint8Array(data.size), received: 0 };
      this.coreStatus(`Downloading core dump · 0 of ${data.size} bytes`);
      this.coreTimeout(); return;
    }
    if (data.type !== 'logs') return;
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
