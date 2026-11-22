// Motion detection, SD recordings, and the preview overlay. Shared by the
// board's local viewer and the React website; both forward data-channel
// messages to each element's handle() and binary chunks to recordings.binary().
// Elements request commands with a bubbling 'camera-command' event.

export const FILE_MAGIC = 0x454c4946;  // "FILE": recording download chunks.

// Incremental Motion-JPEG AVI parser. Push bytes in file order as they arrive;
// each frame is listed as soon as it is complete, so playback can start
// before the download ends. Scans chunks rather than the index, so clips cut
// short by a power loss still play.
export class AviStream {
  constructor(size) {
    this.bytes = new Uint8Array(size);
    this.view = new DataView(this.bytes.buffer);
    this.received = 0; this.position = 12; this.checked = false; this.ended = false;
    this.frames = []; this.microPerFrame = 0; this.totalFrames = 0; this.width = 0; this.height = 0;
  }
  get complete() { return this.received >= this.bytes.length; }
  get frameMicros() { return this.microPerFrame || 100000; }
  push(chunk) {
    if (this.received + chunk.length > this.bytes.length) throw Error('Recording is larger than announced');
    this.bytes.set(chunk, this.received);
    this.received += chunk.length;
    this.scan();
  }
  text(at) { const b = this.bytes; return String.fromCharCode(b[at], b[at + 1], b[at + 2], b[at + 3]); }
  scan() {
    const available = this.received;
    if (!this.checked) {
      if (available < 12) return;
      if (this.text(0) !== 'RIFF' || this.text(8) !== 'AVI ') throw Error('Not an AVI file');
      this.checked = true;
    }
    while (!this.ended && this.position + 8 <= available) {
      const at = this.position, id = this.text(at), size = this.view.getUint32(at + 4, true);
      if (id === 'LIST') {
        if (at + 12 > available) return;
        const type = this.text(at + 8);
        // Descend into header and movi lists; skip any other list.
        this.position += type === 'hdrl' || type === 'strl' || type === 'movi' ? 12 : 8 + size + (size & 1);
        continue;
      }
      if (id === 'idx1') { this.ended = true; return; }
      if (at + 8 + size > available) return;  // Wait for the whole chunk.
      if (id === 'avih' && size >= 40) {
        this.microPerFrame = this.view.getUint32(at + 8, true);
        this.totalFrames = this.view.getUint32(at + 24, true);
        this.width = this.view.getUint32(at + 40, true);
        this.height = this.view.getUint32(at + 44, true);
      } else if (id === '00dc' && size > 0) this.frames.push({ offset: at + 8, length: size });
      this.position += 8 + size + (size & 1);
    }
  }
}

// Frame positions of a complete (or truncated) Motion-JPEG AVI.
export function aviFrames(bytes) {
  const stream = new AviStream(bytes.length);
  stream.push(bytes);
  return { frames: stream.frames, microPerFrame: stream.frameMicros, width: stream.width, height: stream.height };
}

// Clips are named YYYYMMDD/HHMMSS[A-Z].AVI in UTC. Returns null when the
// camera's clock was not set when it recorded.
export function clipStart(name) {
  const match = /^(\d{4})(\d{2})(\d{2})\/(\d{2})(\d{2})(\d{2})[A-Z]?\.AVI$/i.exec(name || '');
  if (!match) return null;
  const [year, month, day, hour, minute, second] = match.slice(1).map(Number);
  if (year < 2023) return null;
  return new Date(Date.UTC(year, month - 1, day, hour, minute, second));
}

export function downloadName(name) {
  const start = clipStart(name);
  if (!start) return `camera-${name.replace(/[^0-9A-Za-z]+/g, '-').replace(/-AVI$/i, '')}.avi`;
  const pad = value => String(value).padStart(2, '0');
  return `camera-${start.getFullYear()}-${pad(start.getMonth() + 1)}-${pad(start.getDate())}_${pad(start.getHours())}-${pad(start.getMinutes())}-${pad(start.getSeconds())}.avi`;
}

export function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return '';
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  if (bytes < 1024 ** 3) return `${(bytes / 1024 ** 2).toFixed(1)} MB`;
  return `${(bytes / 1024 ** 3).toFixed(2)} GB`;
}

export function formatDuration(seconds) {
  const total = Math.max(0, Math.round(seconds || 0));
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`;
}

function cardText(sd) {
  if (!sd) return '';
  if (sd.state === 'mounted') return sd.free_mb !== undefined ? `SD card · ${formatBytes(sd.free_mb * 1024 * 1024)} free of ${formatBytes(sd.total_mb * 1024 * 1024)}` : 'SD card ready';
  return ({ unsupported: 'This board has no SD card slot', absent: 'No SD card inserted', error: 'SD card unreadable · format it here or use a FAT32 card', formatting: 'Formatting SD card… recording is paused' })[sd.state] || 'SD card unavailable';
}

const STYLE = `
  :host{display:block;color:#e4eadd;font:14px system-ui,sans-serif}:host([hidden]){display:none}*{box-sizing:border-box}[hidden]{display:none!important}
  section{background:#17201a;border:1px solid #2a352c;border-radius:8px;padding:24px;min-width:0}
  h2{font-size:20px;font-weight:500;margin:0}h3{font-size:15px;font-weight:600;margin:0 0 14px;color:#c5ed9f}
  header{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px}
  button,input,select{font:inherit;min-height:44px;border-radius:6px;padding:10px;border:1px solid #415335}
  input,select{background:#0d180e;color:#d8e8cb;width:100%;min-width:0}
  button{cursor:pointer;background:#00d4cb;color:#07302c;font-weight:600}button.secondary{background:transparent;color:#d8e8cb}
  button.danger{background:transparent;color:#f0b4a4;border-color:#6a3d33}button:disabled{opacity:.5;cursor:wait}
  input:focus-visible,select:focus-visible,button:focus-visible{outline:2px solid #00d4cb;outline-offset:3px}
  [role=status]{margin:16px 0 0;line-height:1.6;color:#b7c9ab}.muted{color:#9fb293;font-size:13px;line-height:1.5}
  .pills{display:flex;flex-wrap:wrap;gap:8px}.pill{display:inline-flex;align-items:center;gap:6px;border:1px solid #34402f;border-radius:999px;padding:6px 11px;font-size:13px;color:#b7c9ab}
  .pill.on{background:#3a2a12;border-color:#8a6421;color:#ffd58a}.pill.rec{background:#3b1717;border-color:#8c2f2f;color:#ffb1a6}.pill.rec::before{content:'';width:8px;height:8px;border-radius:50%;background:#ff5b4d}
  .actions{display:flex;flex-wrap:wrap;gap:12px}
  @media(max-width:600px){section{padding:18px}input,select{font-size:16px}.actions button{flex:1}}
`;

function command(element, detail) {
  element.dispatchEvent(new CustomEvent('camera-command', { detail, bubbles: true, composed: true }));
}

function defineElements() {
  // group="detection" or group="recording" shows one group of the camera's
  // motion settings (the SD card tab holds the recording group); without it,
  // all groups. Each panel saves and resets only its own fields.
  const PANELS = {
    detection: { title: 'Motion detection', saved: 'Motion settings saved on camera.' },
    recording: { title: 'Recording settings', saved: 'Recording settings saved on camera.' },
  };
  class MotionSettings extends HTMLElement {
    constructor() { super(); this.attachShadow({ mode: 'open' }); this.dirty = false; this.pending = false; }
    get panel() { return PANELS[this.getAttribute('group')] || PANELS.detection; }
    fields() { const group = this.getAttribute('group'); return (this.description?.fields || []).filter(field => !group || field.group === group); }
    shows(group) { const own = this.getAttribute('group'); return !own || own === group; }
    // Every valid field's current value; invalid ones keep the camera's value.
    collect() {
      const form = this.shadowRoot.querySelector('form'), settings = {};
      for (const field of this.fields()) {
        const control = form.elements[field.name];
        if (!control || (field.type === 'number' && (!control.checkValidity() || control.value === ''))) continue;
        settings[field.name] = field.type === 'boolean' ? control.checked : field.type === 'select' ? field.options[Number(control.value)].value : Number(control.value);
      }
      return settings;
    }
    reset() { this.description = null; this.status = null; this.dirty = false; this.pending = false; this.hidden = true; this.shadowRoot.innerHTML = ''; }
    handle(message) {
      if (message.type === 'motion') {
        this.description = message;
        this.hidden = false;
        const idle = 'Changes apply right away. Select Save to keep them after reboot.';
        if (message.live) { if (this.dirty) this.say(message.error || 'Live changes. Select Save to keep them after reboot.'); return; }
        // Replies to another panel's save: keep unsaved edits, otherwise show the camera's values.
        if (!this.pending) { if (!this.dirty) this.render(idle); return; }
        this.pending = false;
        if (message.error) { this.setBusy(false); this.say(message.error); return; }
        this.dirty = false;
        this.render(message.saved ? this.panel.saved : idle);
      } else if (message.type === 'motion_status') {
        this.status = message;
        this.renderStatus();
      }
    }
    say(text) { const status = this.shadowRoot.querySelector('form > .save-status'); if (status) status.textContent = text; }
    setBusy(busy) {
      this.shadowRoot.querySelectorAll('form button,form input,form select').forEach(control => {
        if (busy) { control.dataset.wasDisabled = String(control.disabled); control.disabled = true; } else control.disabled = control.dataset.wasDisabled === 'true';
      });
    }
    showLimit(control, max) {
      control.max = max;
      const note = control.closest('label')?.querySelector('.limit');
      const unit = this.description?.fields.find(field => field.name === control.name)?.unit || '';
      if (note) note.textContent = max > 0 ? `Up to ${max} ${unit} now, limited by ${note.dataset.reason}.` : `Not available now: ${note.dataset.reason} leave no room. Lower the recording rate or resolution.`;
      // Keep an untouched field valid when the limit drops, e.g. after a resolution change.
      if (!this.dirty && Number(control.value) > max) control.value = max;
    }
    renderRecordingWarning() {
      const warning = this.shadowRoot.querySelector('#recording-warning');
      if (!warning) return;
      const record = this.shadowRoot.querySelector('input[name=record]');
      const sd = this.status?.sd;
      const message = record?.checked && sd && sd.state !== 'mounted'
        ? `${cardText(sd)}. The camera can't record clips right now.`
        : '';
      if (warning.textContent !== message) warning.textContent = message;
    }
    renderStatus() {
      this.renderRecordingWarning();
      const box = this.shadowRoot.querySelector('.live');
      if (!box || !this.status) return;
      const s = this.status;
      for (const [name, max] of Object.entries(s.limits || {})) {
        const control = this.shadowRoot.querySelector(`form [name="${CSS.escape(name)}"]`);
        if (control && Number(control.max) !== max) this.showLimit(control, max);
      }
      const pills = [], notes = [];
      const recording = this.description?.values?.record;
      if (this.shows('detection')) {
        if (!s.enabled) pills.push(['Detection off', '']);
        else pills.push([s.motion ? 'Motion detected' : 'No motion', s.motion ? 'on' : ''], [`Moving area ${(s.area ?? 0).toFixed(1)}%`, ''], [`Speed ${(s.speed ?? 0).toFixed(1)} px/s`, ''], [`${(s.fps ?? 0).toFixed(1)} analyses/s`, '']);
        if (s.analysis_width && s.analysis_height) pills.push([`Analysis ${s.analysis_width} × ${s.analysis_height} px`, '']);
      }
      if (this.shows('recording')) {
        if (s.recording) pills.push([`Recording ${formatDuration(s.clip_seconds)}`, 'rec']);
        // The recordings list below shows the card itself; the full panel says it here.
        if (!this.getAttribute('group')) notes.push(cardText(s.sd));
        if (!s.enabled && recording && this.getAttribute('group')) notes.push('Clips are recorded only while motion detection is on (Motion detection tab)');
        if (s.recorder_error) notes.push(s.recorder_error);
        if (s.clock === false) notes.push('Camera clock not set yet; clips are named by uptime until it syncs.');
      }
      box.hidden = !pills.length;
      box.replaceChildren(...pills.map(([text, kind]) => { const pill = document.createElement('span'); pill.className = `pill ${kind}`; pill.textContent = text; return pill; }));
      const card = this.shadowRoot.querySelector('.card');
      card.textContent = notes.filter(Boolean).join(' · ');
      card.hidden = !card.textContent;
    }
    render(message) {
      const description = this.description;
      this.shadowRoot.innerHTML = `<style>${STYLE}
        fieldset{border:1px solid #2a352c;border-radius:8px;padding:18px;margin:0 0 18px;min-width:0}legend{padding:0 6px;color:#c5ed9f;font-weight:600}
        .fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,20rem),1fr));gap:16px}
        label{min-width:0}label small{color:#8fa585;font-size:12px;line-height:1.4}label small.limit{color:#c9b47c}
        .with-unit{display:flex;align-items:center;gap:8px}.with-unit span{color:#9fb293;white-space:nowrap}
        label.field-row{display:grid;grid-template-columns:minmax(0,1fr) auto;column-gap:12px;row-gap:4px;align-items:center;align-content:start}
        label.field-row>small{grid-column:1/-1}label.field-row .title{overflow-wrap:anywhere}
        label.select-row{grid-template-columns:auto minmax(0,1fr)}
        label.counter input{width:88px;flex:none;text-align:right;font-variant-numeric:tabular-nums}label.counter .with-unit span{min-width:4.5ch}
        label.toggle{display:flex;flex-wrap:wrap;align-items:center;gap:10px;min-height:44px}label.toggle input{width:24px;height:24px;min-height:0;margin:0;accent-color:#00d4cb;flex:none}
        label.toggle small{flex-basis:100%;padding-left:34px}.live{margin-bottom:6px}.card{margin:0 0 18px}
        :host([group=recording]){margin-bottom:18px}.groups.fields{margin-bottom:18px}
        #recording-warning{color:#ffd58a;margin:0}#recording-warning:not(:empty){background:#3a2a12;border:1px solid #8a6421;border-radius:6px;padding:10px 12px;margin-bottom:12px}
      </style><section><header><h2></h2></header><div class="live pills" aria-live="polite"></div><p class="card muted"></p>
      <form><div class="groups"></div>${this.shows('recording') ? '<p id="recording-warning" role="status"></p>' : ''}<div class="actions"><button type="submit">Save</button><button type="button" class="secondary">Reset to Default</button></div><p class="save-status" role="status"></p></form></section>`;
      this.shadowRoot.querySelector('h2').textContent = this.panel.title;
      const groups = this.shadowRoot.querySelector('.groups');
      const titles = { detection: 'Detection', recording: 'SD card recording' };
      const sets = {};
      const single = Boolean(this.getAttribute('group'));
      if (single) groups.classList.add('fields');
      for (const field of this.fields()) {
        if (single && !sets[field.group]) sets[field.group] = groups;
        if (!sets[field.group]) {
          const set = document.createElement('fieldset');
          const legend = document.createElement('legend'); legend.textContent = titles[field.group] || field.group;
          const fields = document.createElement('div'); fields.className = 'fields';
          set.append(legend, fields); groups.append(set); sets[field.group] = fields;
        }
        const value = description.values[field.name];
        const label = document.createElement('label');
        let control;
        if (field.type === 'boolean') {
          label.className = 'toggle';
          control = document.createElement('input'); control.type = 'checkbox'; control.checked = Boolean(value);
          label.append(control, document.createTextNode(field.label));
        } else {
          const text = document.createElement('span'); text.className = 'title'; text.textContent = field.label;
          label.append(text);
          if (field.type === 'select') {
            label.className = 'field-row select-row';
            control = document.createElement('select');
            field.options.forEach((option, index) => { const item = document.createElement('option'); item.value = String(index); item.textContent = field.name === 'method' && option.value === 'optical_flow' ? 'Optical flow' : option.label; control.append(item); });
            control.value = String(Math.max(0, field.options.findIndex(option => option.value === value)));
          } else {
            // Values are one to four characters: keep them on the label's line.
            label.className = 'field-row counter';
            control = document.createElement('input'); control.type = 'number'; control.inputMode = 'decimal';
            control.min = field.min; control.max = field.max; control.step = field.step; control.value = value; control.required = true;
          }
          if (control.type === 'number' && field.unit) { const row = document.createElement('span'); row.className = 'with-unit'; const unit = document.createElement('span'); unit.textContent = field.unit; row.append(control, unit); label.append(row); } else label.append(control);
        }
        control.name = field.name;
        control.setAttribute('aria-label', field.label);
        if (field.name === 'record') control.setAttribute('aria-describedby', 'recording-warning');
        if (field.help) { const help = document.createElement('small'); help.textContent = field.help; label.append(help); }
        if (field.max_reason) {
          // The camera computes this maximum; it follows resolution and memory.
          const limit = document.createElement('small'); limit.className = 'limit'; limit.dataset.reason = field.max_reason;
          label.append(limit);
          this.showLimit(control, field.max);
        }
        control.addEventListener('input', () => {
          this.dirty = true;
          if (field.name === 'record') this.renderRecordingWarning();
          // Apply live, as camera settings do; Save also stores them.
          if (this.pending || !control.checkValidity() || control.value === '') return;
          command(this, { op: 'motion_set', settings: this.collect() });
        });
        sets[field.group].append(label);
      }
      this.say(message);
      this.renderStatus();
      const form = this.shadowRoot.querySelector('form');
      const send = detail => {
        if (this.pending) return;
        this.pending = true; this.dirty = true; this.setBusy(true); this.say('Saving on camera…');
        command(this, detail);
      };
      form.onsubmit = event => {
        event.preventDefault();
        if (!form.reportValidity()) return;
        send({ op: 'motion_apply', settings: this.collect() });
      };
      form.querySelector('button[type=button]').onclick = () => {
        // A one-group panel restores only its own fields from the camera's defaults.
        const defaults = description.defaults;
        if (!single || !defaults) { send({ op: 'motion_reset' }); return; }
        send({ op: 'motion_apply', settings: Object.fromEntries(this.fields().filter(field => field.name in defaults).map(field => [field.name, defaults[field.name]])) });
      };
    }
  }

  class Recordings extends HTMLElement {
    constructor() { super(); this.attachShadow({ mode: 'open' }); this.list = null; this.transfer = null; this.cache = null; this.player = null; this.recording = false; this.formatting = false; }
    onVisibilityChange = () => { void this.keepScreenAwake(); };
    connectedCallback() { document.addEventListener('visibilitychange', this.onVisibilityChange); }
    disconnectedCallback() {
      document.removeEventListener('visibilitychange', this.onVisibilityChange);
      this.finishTransfer();
    }
    async keepScreenAwake() {
      const transfer = this.transfer;
      if (!transfer || (transfer.purpose !== 'save' && !transfer.saveWhenDone) ||
          transfer.wakeLockPending || (transfer.wakeLock && !transfer.wakeLock.released) ||
          document.visibilityState !== 'visible' || !navigator.wakeLock) return;
      transfer.wakeLockPending = true;
      try {
        const lock = await navigator.wakeLock.request('screen');
        if (this.transfer === transfer && document.visibilityState === 'visible') transfer.wakeLock = lock;
        else await lock.release();
      } catch { /* The browser can deny a wake lock while the download continues. */ }
      finally { transfer.wakeLockPending = false; }
    }
    finishTransfer() {
      const transfer = this.transfer;
      this.transfer = null;
      if (transfer?.wakeLock) void transfer.wakeLock.release().catch(() => {});
      return transfer;
    }
    reset() {
      this.stopPlayer();
      this.finishTransfer();
      this.list = null; this.cache = null; this.recording = false; this.formatting = false;
      this.hidden = true; this.shadowRoot.innerHTML = '';
    }
    handle(message) {
      if (message.type === 'recordings') {
        this.list = message; this.hidden = false;
        if (message.error && this.formatting) this.formatting = false;  // The camera refused to format.
        // Keep an open confirmation dialog; render once it closes.
        if (this.shadowRoot.querySelector('dialog[open]')) return;
        this.render(); if (message.error) this.say(message.error);
      }
      else if (message.type === 'download') this.downloadStarted(message);
      else if (message.type === 'motion_status') {
        if (this.formatting && message.sd?.state !== 'formatting' && message.format_result) {
          // The camera clears format_result when a format starts, so its return marks the end.
          this.formatting = false;
          this.request(0);
          setTimeout(() => this.say(message.format_result), 300);
        } else if (this.list && message.sd && message.sd.state !== this.list.sd?.state) {
          this.list.sd = message.sd;
          const card = this.shadowRoot.querySelector('.card');
          if (card) card.textContent = cardText(message.sd);
        }
        // Refresh once a clip finishes so it appears with its final length.
        if (this.recording && !message.recording && this.list) this.request(this.list.offset || 0);
        this.recording = Boolean(message.recording);
      }
    }
    request(offset = 0) { command(this, { op: 'recordings', offset }); }
    confirmFormat() {
      const dialog = this.shadowRoot.querySelector('dialog');
      dialog.returnValue = '';
      dialog.onclose = () => {
        if (dialog.returnValue === 'format') {
          this.formatting = true;
          this.stopPlayer(); this.cache = null;
          command(this, { op: 'sd_format' });
          this.render();
          this.say('Formatting SD card… this can take a minute.');
        } else this.render();
      };
      dialog.showModal();
      dialog.querySelector('.keep').focus();
    }
    say(text) { const status = this.shadowRoot.querySelector('section > [role=status]'); if (status) status.textContent = text; }
    // Returns true when the chunk belongs to a recording download.
    binary(buffer) {
      if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 16) return false;
      const view = new DataView(buffer);
      if (view.getUint32(0, true) !== FILE_MAGIC) return false;
      const transfer = this.transfer;
      const id = view.getUint32(4, true), total = view.getUint32(8, true), offset = view.getUint32(12, true);
      if (!transfer?.stream || transfer.id !== id || total !== transfer.size || offset !== transfer.received) return true;
      try { transfer.stream.push(new Uint8Array(buffer, 16)); }
      catch (error) { this.cancel(); this.say(error.message); return true; }
      transfer.received = transfer.stream.received;
      this.progress();
      if (this.player?.stream === transfer.stream) this.framesArrived();
      if (transfer.stream.complete) this.downloadFinished();
      return true;
    }
    downloadStarted(message) {
      const transfer = this.transfer;
      if (!transfer || (message.name !== transfer.name && (message.id === undefined || message.id !== transfer.id))) return;
      if (message.error) { this.finishTransfer(); this.render(); this.say(message.error); return; }
      if (message.id === undefined) return;
      Object.assign(transfer, { id: message.id, size: message.size, received: 0, stream: new AviStream(message.size) });
      if (transfer.purpose === 'play') this.play(transfer.name, transfer.stream);  // Starts with the first frame.
      else this.progress();
    }
    downloadFinished() {
      const transfer = this.finishTransfer();
      const blob = new Blob([transfer.stream.bytes], { type: 'video/x-msvideo' });
      this.cache = { name: transfer.name, blob, stream: transfer.stream };
      this.render();
      if (transfer.purpose === 'save' || transfer.saveWhenDone) this.save(transfer.name, blob);
      if (this.player?.stream === transfer.stream) this.framesArrived();
    }
    save(name, blob) {
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url; link.download = downloadName(name);
      this.shadowRoot.append(link); link.click(); link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
      this.say(`Saved ${link.download}`);
    }
    fetch(name, purpose) {
      if (this.cache?.name === name) { if (purpose === 'save') this.save(name, this.cache.blob); else this.play(name, this.cache.stream); return; }
      if (this.transfer) {
        // Saving the clip that is still streaming into the player: save it when it completes.
        if (purpose === 'save' && this.transfer.name === name) {
          this.transfer.saveWhenDone = true;
          void this.keepScreenAwake();
          this.say('The download starts when the clip has finished loading.');
        }
        return;
      }
      this.transfer = { name, purpose, id: undefined, size: 0, received: 0, stream: null };
      void this.keepScreenAwake();
      this.render();
      command(this, { op: 'recording_download', name });
    }
    cancel() {
      if (!this.transfer) return;
      const { stream } = this.finishTransfer();
      command(this, { op: 'recording_cancel' });
      if (stream && this.player?.stream === stream) this.stopPlayer();
      this.render();
      this.say('Download cancelled.');
    }
    progress() {
      const bar = this.shadowRoot.querySelector('progress'), text = this.shadowRoot.querySelector('.transfer-text');
      const transfer = this.transfer;
      if (!bar || !transfer) return;
      bar.max = transfer.size || 1; bar.value = transfer.received;
      const what = transfer.purpose === 'play' ? 'Loading clip' : 'Downloading';
      text.textContent = transfer.size ? `${what} · ${formatBytes(transfer.received)} of ${formatBytes(transfer.size)} · live preview paused` : 'Starting…';
    }
    stopPlayer() {
      if (!this.player) return;
      clearTimeout(this.player.timer);
      if (this.player.url) URL.revokeObjectURL(this.player.url);
      this.player = null;
    }
    // Plays a stream that may still be downloading.
    play(name, stream) {
      this.stopPlayer();
      if (stream.complete && !stream.frames.length) { this.say('This clip has no frames.'); return; }
      this.player = { name, stream, index: 0, shown: -1, playing: true, waiting: false, timer: 0, url: '' };
      this.render();
      this.framesArrived();
    }
    loading() { return Boolean(this.player && this.transfer?.stream === this.player.stream); }
    framesArrived() {
      const player = this.player;
      if (!player) return;
      const range = this.shadowRoot.querySelector('.player input[type=range]');
      if (range) range.max = Math.max(0, (player.stream.totalFrames || player.stream.frames.length) - 1);
      if (player.shown < 0 && player.stream.frames.length) { this.showFrame(0); this.schedule(); }
      else if (player.waiting && player.index + 1 < player.stream.frames.length) { player.waiting = false; this.schedule(); }
      else if (player.shown < 0 || player.waiting) this.updateTime();
      if (player.stream.complete && !player.stream.frames.length) { this.stopPlayer(); this.render(); this.say('This clip has no frames.'); }
    }
    updateTime() {
      const player = this.player, time = this.shadowRoot.querySelector('.player .time');
      if (!player || !time) return;
      if (player.shown < 0) { time.textContent = 'Loading clip…'; return; }
      const micros = player.stream.frameMicros, frames = player.stream.totalFrames || player.stream.frames.length;
      const seconds = player.index * micros / 1e6, length = frames * micros / 1e6;
      const start = clipStart(player.name);
      time.textContent = `${formatDuration(seconds)} / ${formatDuration(length)}${start ? ' · ' + new Date(start.getTime() + seconds * 1000).toLocaleTimeString() : ''}${player.waiting ? ' · Buffering…' : ''}`;
    }
    showFrame(index) {
      const player = this.player;
      if (!player || !player.stream.frames.length) return;
      const frames = player.stream.frames;
      player.index = Math.min(Math.max(0, index), frames.length - 1);
      player.shown = player.index;
      const frame = frames[player.index];
      const url = URL.createObjectURL(new Blob([player.stream.bytes.subarray(frame.offset, frame.offset + frame.length)], { type: 'image/jpeg' }));
      const image = this.shadowRoot.querySelector('.player img');
      const previous = player.url;
      player.url = url;
      if (image) image.src = url;
      if (previous) URL.revokeObjectURL(previous);
      const range = this.shadowRoot.querySelector('.player input[type=range]');
      if (range) range.value = player.index;
      this.updateTime();
    }
    schedule() {
      const player = this.player;
      if (!player || !player.playing) return;
      clearTimeout(player.timer);
      player.timer = setTimeout(() => {
        if (this.player !== player) return;
        if (player.index + 1 < player.stream.frames.length) { this.showFrame(player.index + 1); this.schedule(); }
        else if (this.loading()) { player.waiting = true; this.updateTime(); }  // Resumes in framesArrived().
        else { player.playing = false; this.updatePlayButton(); }
      }, player.stream.frameMicros / 1000);
    }
    updatePlayButton() { const button = this.shadowRoot.querySelector('.player .play'); if (button && this.player) button.textContent = this.player.playing ? 'Pause' : 'Play'; }
    render() {
      const list = this.list;
      if (!list) return;
      this.shadowRoot.innerHTML = `<style>${STYLE}
        ul{list-style:none;margin:0;padding:0;display:grid;gap:10px}
        li{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:12px;align-items:center;border:1px solid #2a352c;border-radius:8px;padding:12px 14px;background:#121a15}
        li strong{display:block;font-weight:600;overflow-wrap:anywhere}li .actions{gap:8px}li .actions button{min-width:88px}
        .badge{display:inline-block;margin-left:8px;font-size:12px;border-radius:999px;padding:2px 8px;border:1px solid #8c2f2f;color:#ffb1a6}
        .transfer{display:grid;gap:8px;margin:0 0 16px}progress{width:100%;height:12px;accent-color:#00d4cb}
        .player{display:grid;gap:12px;margin:0 0 18px;padding:14px;border:1px solid #2a352c;border-radius:8px;background:#0a110d}
        .player img{display:block;width:100%;max-height:60vh;object-fit:contain;background:#000;border-radius:4px}
        .player input[type=range]{padding:0;accent-color:#00d4cb;border:none;background:transparent}.player .actions{align-items:center}.time{font-variant-numeric:tabular-nums;color:#b7c9ab}
        .pages{display:flex;flex-wrap:wrap;gap:12px;margin-top:16px;align-items:center}
        dialog{background:#17201a;color:#e4eadd;border:1px solid #8c4a3d;border-radius:10px;padding:22px;width:min(440px,calc(100vw - 32px))}dialog::backdrop{background:#000b}
        dialog h3{color:#ffb1a6;font-size:18px}dialog p{line-height:1.6;color:#d0dcc6;margin:0 0 18px}button.erase{background:#c4473a;border-color:#c4473a;color:#fff}
        @media(max-width:600px){li{grid-template-columns:1fr}li .actions button{flex:1;min-width:0}header .actions{width:100%}}
      </style><section><header><div><h2>Recordings</h2><p class="muted card"></p></div><div class="actions"><button type="button" class="secondary refresh">Refresh</button><button type="button" class="danger format">Format SD card</button></div></header>
      <dialog aria-labelledby="format-title" aria-describedby="format-text"><form method="dialog"><h3 id="format-title">Format the SD card?</h3><p id="format-text">This erases everything on the card, including all recordings and any other files, and creates a new FAT volume. It cannot be undone. Recording pauses until it finishes.</p><div class="actions"><button value="cancel" class="secondary keep">Cancel</button><button value="format" class="erase">Erase and format</button></div></form></dialog>
      <div class="transfer" hidden><progress value="0" max="1"></progress><div class="actions"><span class="transfer-text muted"></span><button type="button" class="secondary cancel">Cancel</button></div></div>
      <div class="player" hidden><img alt="Recorded clip"><input type="range" min="0" value="0" aria-label="Clip position"><div class="actions"><button type="button" class="play">Pause</button><button type="button" class="secondary save-clip">Download</button><button type="button" class="secondary close">Close</button><span class="time"></span></div></div>
      <ul></ul><div class="pages"></div><p role="status"></p></section>`;
      const root = this.shadowRoot;
      root.querySelector('.card').textContent = cardText(list.sd);
      root.querySelector('.refresh').onclick = () => this.request(list.offset || 0);
      const format = root.querySelector('.format');
      format.disabled = this.formatting || Boolean(this.transfer) || !['mounted', 'error'].includes(list.sd?.state);
      format.onclick = () => this.confirmFormat();
      if (this.transfer) {
        root.querySelector('.transfer').hidden = false;
        root.querySelector('.cancel').onclick = () => this.cancel();
        this.progress();
      }
      if (this.player) {
        const player = this.player, box = root.querySelector('.player');
        box.hidden = false;
        const range = box.querySelector('input[type=range]');
        range.max = Math.max(0, (player.stream.totalFrames || player.stream.frames.length) - 1);
        range.oninput = () => { player.playing = false; player.waiting = false; clearTimeout(player.timer); this.updatePlayButton(); this.showFrame(Number(range.value)); };
        box.querySelector('.play').onclick = () => {
          player.playing = !player.playing;
          player.waiting = false;
          if (player.playing && !this.loading() && player.index + 1 >= player.stream.frames.length) this.showFrame(0);
          this.updatePlayButton();
          if (player.playing) this.schedule(); else clearTimeout(player.timer);
        };
        box.querySelector('.save-clip').onclick = () => this.fetch(player.name, 'save');
        box.querySelector('.close').onclick = () => {
          if (this.loading()) this.cancel();  // Stop loading a clip nobody is watching.
          this.stopPlayer(); this.render();
        };
        this.updatePlayButton();
        if (player.shown >= 0) this.showFrame(player.index); else this.updateTime();
      }
      const items = root.querySelector('ul');
      for (const file of list.files) {
        const item = document.createElement('li');
        const info = document.createElement('div');
        const title = document.createElement('strong');
        const start = clipStart(file.name);
        title.textContent = start ? start.toLocaleString() : `${file.name} (clock not set)`;
        if (file.recording) { const badge = document.createElement('span'); badge.className = 'badge'; badge.textContent = 'Recording'; title.append(badge); }
        const details = document.createElement('span'); details.className = 'muted';
        details.textContent = [file.duration ? formatDuration(file.duration) : file.complete === false ? 'Incomplete clip' : '', file.width ? `${file.width} × ${file.height}` : '', formatBytes(file.size)].filter(Boolean).join(' · ');
        info.append(title, details);
        const actions = document.createElement('div'); actions.className = 'actions';
        const button = (text, kind, handler) => { const control = document.createElement('button'); control.type = 'button'; control.textContent = text; if (kind) control.className = kind; control.disabled = Boolean(file.recording || this.transfer); control.onclick = handler; control.setAttribute('aria-label', `${text} ${title.textContent}`); actions.append(control); };
        button('Play', '', () => this.fetch(file.name, 'play'));
        button('Download', 'secondary', () => this.fetch(file.name, 'save'));
        button('Delete', 'danger', () => {
          if (!confirm(`Delete the clip from ${title.textContent}?`)) return;
          if (this.cache?.name === file.name) this.cache = null;
          if (this.player?.name === file.name) this.stopPlayer();
          command(this, { op: 'recording_delete', name: file.name });
        });
        item.append(info, actions);
        items.append(item);
      }
      if (!list.files.length) {
        const empty = document.createElement('li'); empty.className = 'muted';
        empty.textContent = list.sd?.state === 'mounted' ? 'No recordings yet. Enable motion detection and SD card recording above.' : cardText(list.sd);
        items.append(empty);
      }
      const pages = root.querySelector('.pages');
      const offset = list.offset || 0, page = list.page || 25;
      if (list.total > page) {
        const label = document.createElement('span'); label.className = 'muted';
        label.textContent = `${offset + 1}–${Math.min(offset + page, list.total)} of ${list.total}`;
        const newer = document.createElement('button'); newer.type = 'button'; newer.className = 'secondary'; newer.textContent = 'Newer'; newer.disabled = offset === 0; newer.onclick = () => this.request(Math.max(0, offset - page));
        const older = document.createElement('button'); older.type = 'button'; older.className = 'secondary'; older.textContent = 'Older'; older.disabled = offset + page >= list.total; older.onclick = () => this.request(offset + page);
        pages.append(newer, older, label);
      }
    }
  }

  // Draws moving cells over the live preview. Place it over the preview image
  // with the same box (position:absolute; inset:0 inside a positioned wrapper).
  class MotionOverlay extends HTMLElement {
    constructor() {
      super();
      this.attachShadow({ mode: 'open' }).innerHTML = `<style>:host{display:block;pointer-events:none}canvas{width:100%;height:100%;display:block}
        span{position:absolute;top:8px;left:8px;font:600 12px system-ui,sans-serif;letter-spacing:.5px;padding:4px 8px;border-radius:4px;background:#000a;color:#ffd58a}span[hidden]{display:none}
        span.rec{left:auto;right:8px;color:#ffb1a6}</style><canvas></canvas><span class="motion" hidden>MOTION</span><span class="rec" hidden>● REC</span>`;
    }
    reset() { this.status = null; this.draw(); }
    handle(message) { if (message.type === 'motion_status') { this.status = message; this.draw(); } }
    draw() {
      const canvas = this.shadowRoot.querySelector('canvas');
      const s = this.status;
      this.shadowRoot.querySelector('.motion').hidden = !s?.motion;
      this.shadowRoot.querySelector('.rec').hidden = !s?.recording;
      const width = Math.round(canvas.clientWidth * devicePixelRatio), height = Math.round(canvas.clientHeight * devicePixelRatio);
      if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
      const context = canvas.getContext('2d');
      context.clearRect(0, 0, width, height);
      if (!s?.grid || !s.cols || !s.rows) return;
      const cellWidth = width / s.cols, cellHeight = height / s.rows;
      context.fillStyle = s.motion ? 'rgba(255,170,40,0.35)' : 'rgba(0,212,203,0.25)';
      context.strokeStyle = s.motion ? 'rgba(255,190,80,0.9)' : 'rgba(0,212,203,0.7)';
      context.lineWidth = Math.max(1, devicePixelRatio);
      for (let index = 0; index < s.cols * s.rows; index++) {
        if (!(parseInt(s.grid[index >> 2] || '0', 16) & (8 >> (index & 3)))) continue;
        const x = (index % s.cols) * cellWidth, y = Math.floor(index / s.cols) * cellHeight;
        context.fillRect(x, y, cellWidth, cellHeight);
        context.strokeRect(x + 0.5, y + 0.5, cellWidth - 1, cellHeight - 1);
      }
    }
  }

  if (!customElements.get('sensorfan-motion-settings')) customElements.define('sensorfan-motion-settings', MotionSettings);
  if (!customElements.get('sensorfan-recordings')) customElements.define('sensorfan-recordings', Recordings);
  if (!customElements.get('sensorfan-motion-overlay')) customElements.define('sensorfan-motion-overlay', MotionOverlay);
}

if (globalThis.customElements) defineElements();
