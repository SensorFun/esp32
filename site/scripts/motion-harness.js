// Stands in for the camera in check-browser.py: answers the motion and
// recording commands that the shared camera_ui elements dispatch, as the
// firmware would over the data channel. No board or WebRTC connection is used.
(() => {
  const fields = [
    { name: 'enabled', label: 'Detect motion', group: 'detection', type: 'boolean', help: 'Analyse the camera continuously.' },
    { name: 'method', label: 'Method', group: 'detection', type: 'select', help: 'Detection algorithm.', options: [{ value: 'optical_flow', label: 'Optical flow (Lucas-Kanade)' }] },
    { name: 'min_speed', label: 'Minimum speed', group: 'detection', type: 'number', unit: 'px/s', min: 0.25, max: 40, step: 0.25, help: 'Speed that marks a cell as moving.' },
    { name: 'cell', label: 'Cell size', group: 'detection', type: 'select', unit: 'px', help: 'Analysis grid.', options: [5, 8, 10, 20].map(value => ({ value, label: `${value} px` })) },
    { name: 'trigger_seconds', label: 'Motion duration', group: 'detection', type: 'number', unit: 's', min: 0, max: 30, step: 0.5, help: 'Motion must last this long.' },
    { name: 'record', label: 'Record to SD card', group: 'recording', type: 'boolean', help: 'Store clips.' },
    { name: 'pre_seconds', label: 'Pre-record', group: 'recording', type: 'number', unit: 's', min: 0, max: 4, step: 1, help: 'Keep the seconds before motion.', max_reason: 'free PSRAM, the camera resolution, and the recording rate' },
    { name: 'post_seconds', label: 'Keep recording', group: 'recording', type: 'number', unit: 's', min: 1, max: 300, step: 1, help: 'After motion stops.' },
  ];
  const defaults = { enabled: false, method: 'optical_flow', min_speed: 2.5, cell: 8, trigger_seconds: 1, record: true, pre_seconds: 2, post_seconds: 10 };
  const state = { values: { ...defaults, enabled: true }, commands: [], clip: null, files: [], chunkDelay: 120 };
  const sd = { state: 'mounted', total_mb: 30436, free_mb: 29001 };
  const views = () => ['sensorfan-motion-settings', 'sensorfan-recordings', 'sensorfan-motion-overlay'].map(name => document.querySelector(name));
  const deliver = message => setTimeout(() => views().forEach(view => view.handle(message)), 20);
  const list = () => ({ type: 'recordings', files: state.files, total: state.files.length, offset: 0, page: 25, sd });

  async function jpeg(shade) {
    const canvas = document.createElement('canvas'); canvas.width = 320; canvas.height = 240;
    const context = canvas.getContext('2d');
    context.fillStyle = `rgb(${shade},${120 + shade / 3},90)`; context.fillRect(0, 0, 320, 240);
    context.fillStyle = '#fff'; context.fillRect(40 + shade / 2, 90, 60, 60);
    return new Uint8Array(await (await new Promise(resolve => canvas.toBlob(resolve, 'image/jpeg', 0.8))).arrayBuffer());
  }
  // Same layout as the firmware's AviWriter.
  function avi(frames, micro) {
    const parts = [];
    const ascii = text => [...text].map(c => c.charCodeAt(0));
    const u32 = value => { const bytes = new Uint8Array(4); new DataView(bytes.buffer).setUint32(0, value, true); return bytes; };
    const avih = new Uint8Array(56); const view = new DataView(avih.buffer);
    view.setUint32(0, micro, true); view.setUint32(16, frames.length, true); view.setUint32(32, 320, true); view.setUint32(36, 240, true);
    const movi = frames.reduce((sum, frame) => sum + 8 + frame.length + (frame.length & 1), 4);
    parts.push(new Uint8Array([...ascii('RIFF'), 0, 0, 0, 0, ...ascii('AVI ')]));
    parts.push(new Uint8Array([...ascii('LIST'), ...u32(68), ...ascii('hdrl'), ...ascii('avih'), ...u32(56)]), avih);
    parts.push(new Uint8Array([...ascii('LIST'), ...u32(movi), ...ascii('movi')]));
    for (const frame of frames) { parts.push(new Uint8Array([...ascii('00dc'), ...u32(frame.length)]), frame); if (frame.length & 1) parts.push(new Uint8Array(1)); }
    const out = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
    let offset = 0; for (const part of parts) { out.set(part, offset); offset += part.length; }
    return out;
  }

  function answer(detail) {
    state.commands.push(detail);
    if (detail.op === 'motion_get') deliver({ type: 'motion', fields, values: state.values, defaults });
    else if (detail.op === 'motion_apply') { state.values = { ...state.values, ...detail.settings }; deliver({ type: 'motion', fields, values: state.values, defaults, saved: true }); }
    else if (detail.op === 'motion_reset') { state.values = { ...defaults }; deliver({ type: 'motion', fields, values: state.values, defaults, saved: true }); }
    else if (detail.op === 'recordings') deliver(list());
    else if (detail.op === 'sd_format') {
      deliver(list());
      const base = { type: 'motion_status', enabled: true, method: 'optical_flow', motion: false, area: 0, speed: 0, fps: 5, recording: false, clock: true };
      setTimeout(() => deliver({ ...base, sd: { state: 'formatting' } }), 100);
      setTimeout(() => { state.files = []; deliver({ ...base, sd, format_result: 'SD card formatted' }); }, 400);
    }
    else if (detail.op === 'recording_delete') { state.files = state.files.filter(file => file.name !== detail.name); deliver(list()); }
    else if (detail.op === 'recording_download') {
      const id = state.commands.length, bytes = state.clip, recordings = document.querySelector('sensorfan-recordings');
      deliver({ type: 'download', name: detail.name, id, size: bytes.length });
      // Chunks arrive spaced out, like a real link, so playback can start early.
      for (let offset = 0, step = 0; offset < bytes.length; offset += 2048, step++) {
        setTimeout(() => {
          const chunk = bytes.subarray(offset, offset + 2048), packet = new Uint8Array(16 + chunk.length);
          new Uint32Array(packet.buffer, 0, 4).set([0x454c4946, id, bytes.length, offset]);
          packet.set(chunk, 16);
          recordings.binary(packet.buffer);
        }, 60 + step * state.chunkDelay);
      }
    }
  }

  window.motionHarness = {
    state,
    async install() {
      const frames = [];
      for (let shade = 0; shade < 120; shade += 12) frames.push(await jpeg(shade));
      state.clip = avi(frames, 250000);
      state.files = [
        { name: '20261115/142301.AVI', size: 18500000, frames: 300, duration: 30, width: 1280, height: 720, complete: true },
        { name: '20261115/101500.AVI', size: state.clip.length, frames: frames.length, duration: 2.5, width: 320, height: 240, complete: true },
      ];
      document.addEventListener('camera-command', event => answer(event.detail), true);
      for (const view of views()) view.reset?.();
      const preview = document.querySelector('[data-testid=remote-image], #preview');
      preview.src = URL.createObjectURL(new Blob([frames[3]], { type: 'image/jpeg' }));
      await new Promise(resolve => { preview.onload = resolve; });
      answer({ op: 'motion_get' });
      answer({ op: 'recordings' });
    },
    status(extra = {}) {
      // 20 x 15 cells; a 4 x 3 block near the centre moves.
      const bits = Array(300).fill(0);
      for (let row = 6; row < 9; row++) for (let col = 8; col < 12; col++) bits[row * 20 + col] = 1;
      let grid = '';
      for (let i = 0; i < 300; i += 4) grid += ((bits[i] << 3) | (bits[i + 1] << 2) | (bits[i + 2] << 1) | bits[i + 3]).toString(16);
      deliver({ type: 'motion_status', enabled: true, method: 'optical_flow', motion: true, area: 4, speed: 6, fps: 5, cols: 20, rows: 15, grid, recording: true, clip: '20261115/142301.AVI', clip_seconds: 12, clip_frames: 120, sd, clock: true, limits: { pre_seconds: 4 }, ...extra });
    },
  };
})();
