import { test } from 'node:test';
import assert from 'node:assert/strict';
import { AviStream, aviFrames, clipStart, downloadName, formatBytes, formatDuration } from '../../projects/common/camera_ui/motion.js';

// Builds the same layout as the firmware's AviWriter.
function avi(frames, { micro = 100000, finalized = true } = {}) {
  const parts = [];
  const chunk = (id, body) => {
    const header = new Uint8Array(8); header.set([...id].map(c => c.charCodeAt(0)));
    new DataView(header.buffer).setUint32(4, body.length, true);
    parts.push(header, body); if (body.length & 1) parts.push(new Uint8Array(1));
  };
  const avih = new Uint8Array(56); const view = new DataView(avih.buffer);
  view.setUint32(0, micro, true); view.setUint32(16, frames.length, true); view.setUint32(32, 320, true); view.setUint32(36, 240, true);
  const list = (type, size) => { const h = new Uint8Array(12); h.set([...'LIST'].map(c => c.charCodeAt(0))); new DataView(h.buffer).setUint32(4, size, true); h.set([...type].map(c => c.charCodeAt(0)), 8); parts.push(h); };
  const riff = new Uint8Array(12); riff.set([...'RIFF'].map(c => c.charCodeAt(0))); riff.set([...'AVI '].map(c => c.charCodeAt(0)), 8); parts.push(riff);
  list('hdrl', 4 + 64); chunk('avih', avih);
  const movi = frames.reduce((sum, frame) => sum + 8 + frame.length + (frame.length & 1), 4);
  list('movi', movi);
  for (const frame of frames) chunk('00dc', frame);
  if (finalized) chunk('idx1', new Uint8Array(16 * frames.length));
  const out = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
  let offset = 0; for (const part of parts) { out.set(part, offset); offset += part.length; }
  return out;
}

test('AVI frames are found with their timing and size', () => {
  const frames = [new Uint8Array([0xff, 0xd8, 1, 0xff, 0xd9]), new Uint8Array([0xff, 0xd8, 2, 3, 0xff, 0xd9])];
  const bytes = avi(frames, { micro: 200000 });
  const parsed = aviFrames(bytes);
  assert.equal(parsed.microPerFrame, 200000);
  assert.deepEqual([parsed.width, parsed.height], [320, 240]);
  assert.deepEqual(parsed.frames.map(frame => Array.from(bytes.subarray(frame.offset, frame.offset + frame.length))), frames.map(frame => Array.from(frame)));
});

test('clips cut short by power loss still play at the default rate', () => {
  const frames = [new Uint8Array([0xff, 0xd8, 9, 0xff, 0xd9]), new Uint8Array([0xff, 0xd8, 8, 0xff, 0xd9])];
  const bytes = avi(frames, { micro: 0, finalized: false });
  const truncated = bytes.subarray(0, bytes.length - 3);  // The last frame was only partly written.
  const parsed = aviFrames(truncated);
  assert.equal(parsed.microPerFrame, 100000);
  assert.equal(parsed.frames.length, 1);
  assert.throws(() => aviFrames(new Uint8Array(20)), /Not an AVI/);
});

test('clip names give UTC start times and friendly download names', () => {
  assert.equal(clipStart('20261115/142301.AVI').toISOString(), '2026-11-15T14:23:01.000Z');
  assert.equal(clipStart('20261115/142301B.AVI').toISOString(), '2026-11-15T14:23:01.000Z');
  assert.equal(clipStart('19700101/000012.AVI'), null);
  assert.equal(clipStart('../secret.avi'), null);
  assert.match(downloadName('20261115/142301.AVI'), /^camera-2026-11-1\d_\d\d-\d\d-01\.avi$/);
  assert.equal(downloadName('19700101/000012.AVI'), 'camera-19700101-000012.avi');
});

test('sizes and durations are formatted for people', () => {
  assert.equal(formatBytes(5 * 1024 * 1024), '5.0 MB');
  assert.equal(formatBytes(2048), '2 KB');
  assert.equal(formatDuration(75.4), '1:15');
});

test('a clip pushed in pieces lists each frame once it is complete', () => {
  const frames = [new Uint8Array([0xff, 0xd8, 1, 2, 0xff, 0xd9]), new Uint8Array([0xff, 0xd8, 3, 0xff, 0xd9]), new Uint8Array([0xff, 0xd8, 4, 5, 6, 0xff, 0xd9])];
  const bytes = avi(frames, { micro: 125000 });
  const stream = new AviStream(bytes.length);
  const seen = [];
  for (let offset = 0; offset < bytes.length; offset += 7) {
    stream.push(bytes.subarray(offset, offset + 7));
    seen.push(stream.frames.length);
    for (const frame of stream.frames) assert.ok(frame.offset + frame.length <= stream.received);
  }
  assert.ok(stream.complete);
  assert.equal(stream.frameMicros, 125000);
  assert.equal(stream.totalFrames, 3);
  assert.deepEqual([...new Set(seen)], [0, 1, 2, 3]);  // Frames appear one at a time.
  assert.throws(() => stream.push(new Uint8Array(1)), /larger than announced/);
});
