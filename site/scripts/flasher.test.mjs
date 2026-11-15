import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { checkLayout, chipId, firmwareRoots, md5, parseFlasherArgs, portLabel, safePath } from '../src/projects/flasher/plan.mjs';

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

test('chip names, firmware folders, and port labels', () => {
  assert.equal(chipId('ESP32-S3'), 'esp32s3');
  assert.equal(chipId('ESP32'), 'esp32');
  assert.deepEqual(firmwareRoots(['webrtc/esp32_ai-thinker_cam/flasher_args.json', 'webrtc/esp32_ai-thinker_cam/webrtc.bin', 'flasher_args.json', 'x/not_flasher_args.json']), ['', 'webrtc/esp32_ai-thinker_cam']);
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
