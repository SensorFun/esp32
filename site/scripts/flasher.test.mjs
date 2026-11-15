import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { appOffset, checkLayout, chipId, inspectImage, md5, parseFlasherArgs, portLabel, safePath } from '../src/projects/flasher/plan.mjs';

const args = {
  flash_settings: { flash_mode: 'dio', flash_size: '4MB', flash_freq: '80m' },
  flash_files: { '0x10000': 'webrtc.bin', '0x0': 'bootloader/bootloader.bin', '0x8000': 'partition_table/partition-table.bin' },
  extra_esptool_args: { after: 'hard-reset', before: 'default-reset', stub: true, chip: 'esp32s3' },
};

test('flasher_args.json becomes an ordered flash plan', () => {
  const plan = parseFlasherArgs(JSON.stringify(args));
  assert.equal(plan.chip, 'esp32s3');
  assert.deepEqual(plan.files.map(file => [file.address, file.path]), [[0, 'bootloader/bootloader.bin'], [0x8000, 'partition_table/partition-table.bin'], [0x10000, 'webrtc.bin']]);
  assert.deepEqual([plan.flashMode, plan.flashFreq, plan.flashSize, plan.before, plan.after], ['dio', '80m', '4MB', 'default_reset', 'hard_reset']);
});

test('unsafe paths, offsets, and settings are rejected', () => {
  for (const path of ['../secret.bin', '/etc/passwd', 'a//b.bin', 'a\\b.bin', './x.bin', '']) assert.throws(() => safePath(path), /Unsafe/);
  assert.throws(() => parseFlasherArgs(JSON.stringify({ ...args, flash_files: { '0x0': '../x.bin' } })), /Unsafe/);
  assert.throws(() => parseFlasherArgs(JSON.stringify({ ...args, flash_files: { '4096': 'x.bin' } })), /Invalid flash offset/);
  assert.throws(() => parseFlasherArgs(JSON.stringify({ ...args, flash_settings: { flash_mode: 'fast' } })), /Unsupported/);
  assert.throws(() => parseFlasherArgs('{'), /not valid JSON/);
  assert.throws(() => parseFlasherArgs('{}'), /missing/);
});

test('layouts that overlap or exceed the flash size are rejected', () => {
  const plan = parseFlasherArgs(JSON.stringify(args));
  checkLayout(plan, [0x5000, 0xc00, 0x150000]);
  assert.throws(() => checkLayout(plan, [0x9000, 0xc00, 0x1000]), /overlaps/);
  assert.throws(() => checkLayout(plan, [0x5000, 0xc00, 0x400000]), /does not fit/);
});

test('chip names and port labels', () => {
  assert.equal(chipId('ESP32-S3'), 'esp32s3');
  assert.equal(chipId('ESP32'), 'esp32');
  assert.equal(portLabel({ usbVendorId: 0x303a, usbProductId: 0x1001 }), 'Espressif USB (303a:1001)');
  assert.equal(portLabel({ usbVendorId: 0x1a86, usbProductId: 0x7523 }), 'WCH CH340 (1a86:7523)');
  assert.equal(portLabel({ usbVendorId: 0x1234, usbProductId: 1 }), 'USB serial (1234:0001)');
  assert.equal(portLabel({}), 'Serial port');
});

test('MD5 matches Node for empty, block-boundary, and firmware-sized data', () => {
  for (const size of [0, 1, 55, 56, 63, 64, 65, 1000, 1431984]) {
    const data = randomBytes(size);
    assert.equal(md5(new Uint8Array(data)), createHash('md5').update(data).digest('hex'), `size ${size}`);
  }
});

// Minimal ESP image: magic, chip ID at byte 12, optional app descriptor at 0x20.
function image(chipCode, { app = false, padding = 0, size = 4096 } = {}) {
  const bytes = new Uint8Array(padding + size).fill(0xff, 0, padding);
  const view = new DataView(bytes.buffer);
  bytes[padding] = 0xe9;
  view.setUint16(padding + 12, chipCode, true);
  if (app) {
    view.setUint32(padding + 0x20, 0xabcd5432, true);
    bytes.set(new TextEncoder().encode('1.2.3'), padding + 0x30);
    bytes.set(new TextEncoder().encode('webrtc'), padding + 0x50);
  }
  return bytes;
}

test('uploaded app images go to the app partition with their chip and version', () => {
  assert.deepEqual(inspectImage(image(9, { app: true })), { kind: 'app', chip: 'esp32s3', address: appOffset, version: '1.2.3', project: 'webrtc' });
  assert.equal(inspectImage(image(0, { app: true })).chip, 'esp32');
});

function fullImage(chipCode, padding = 0) {
  const offset = !padding && chipCode === 0 ? 0x1000 : 0;
  const full = new Uint8Array(0x12000 - offset).fill(0xff);
  full.set(image(chipCode, { padding }), 0);
  full.set(image(chipCode, { app: true }), 0x10000 - offset);
  return full;
}

test('uploaded full images start at the chip bootloader offset', () => {
  assert.deepEqual(inspectImage(fullImage(9)), { kind: 'full', chip: 'esp32s3', address: 0 });
  assert.deepEqual(inspectImage(fullImage(0)), { kind: 'full', chip: 'esp32', address: 0x1000 });
  assert.deepEqual(inspectImage(fullImage(0, 0x1000)), { kind: 'full', chip: 'esp32', address: 0 });
  assert.throws(() => inspectImage(image(9, { padding: 0x1000 })), /bootloader/);
});

test('files that are not ESP32 firmware are rejected', () => {
  assert.throws(() => inspectImage(new Uint8Array(10), 'x.bin'), /too small/);
  assert.throws(() => inspectImage(new TextEncoder().encode('\x7fELF'.padEnd(200, 'x')), 'app.elf'), /not an ESP32 firmware/);
  assert.throws(() => inspectImage(image(99)), /unsupported chip/);
  assert.throws(() => inspectImage(image(9)), /not a complete flash image/);
  const padded = image(0, { padding: 0x1000 }); padded[5] = 0;
  assert.throws(() => inspectImage(padded), /not an ESP32 firmware/);
});
