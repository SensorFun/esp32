import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { packageFiles, readCatalog, readFirmware, readFirmwareCatalog } from './boards-plugin.mjs';
import { checkBundle } from './check-bundle.mjs';

const workspace = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

function fakeWorkspace() {
  const root = mkdtempSync(path.join(tmpdir(), 'boards-'));
  mkdirSync(path.join(root, 'config/boards/images'), { recursive: true });
  writeFileSync(path.join(root, 'config/boards/images/a.png'), 'png');
  writeFileSync(path.join(root, 'config/boards/maker.yaml'),
    'board_a:\n  name: Board A\n  target: esp32s3\n  image: images/a.png\nboard_b:\n  name: Board B\n  target: esp32\n');
  mkdirSync(path.join(root, 'config/firmware'), { recursive: true });
  writeFileSync(path.join(root, 'config/firmware/webrtc.yaml'), 'name: WebRTC\ndescription: Camera firmware\nboards: all\n');
  return root;
}

function fakePackage(root, board, chip, { isPrivate = false, selectionBoard = board } = {}) {
  const folder = path.join(root, 'firmware/webrtc', board);
  mkdirSync(path.join(folder, 'bootloader'), { recursive: true });
  const files = { '0x0': 'bootloader/bootloader.bin', '0x10000': 'webrtc.bin' };
  writeFileSync(path.join(folder, 'flasher_args.json'), JSON.stringify({ flash_settings: { flash_mode: 'dio', flash_size: '4MB', flash_freq: '80m' }, flash_files: files, extra_esptool_args: { chip, before: 'default-reset', after: 'hard-reset' } }));
  writeFileSync(path.join(folder, 'firmware_selection.json'), JSON.stringify({ board: selectionBoard, frame_grabber: 'ov' }));
  writeFileSync(path.join(folder, 'build.json'), JSON.stringify({ project: 'webrtc', board, commit: 'abc', built: 1, private: isPrivate }));
  for (const file of Object.values(files)) writeFileSync(path.join(folder, file), Buffer.alloc(1024));
  writeFileSync(path.join(folder, 'firmware.bin'), Buffer.alloc(0x10400));
  return folder;
}

test('the real board catalog lists every board with an existing image', () => {
  const boards = readCatalog(workspace);
  assert.ok(boards.length >= 3);
  for (const board of boards) {
    assert.match(board.id, /^[A-Za-z0-9_-]+$/);
    assert.ok(board.name && board.target.startsWith('esp32'));
    assert.ok(board.image && existsSync(board.image), board.id);
  }
});

test('catalog entries are validated', () => {
  const root = fakeWorkspace();
  assert.deepEqual(readCatalog(root).map(board => [board.id, board.image ? path.basename(board.image) : null]), [['board_a', 'a.png'], ['board_b', null]]);
  writeFileSync(path.join(root, 'config/boards/bad.yaml'), 'board_c:\n  name: C\n  target: esp32\n  image: ../maker.yaml\n');
  assert.throws(() => readCatalog(root), /image/);
});

test('the firmware catalog resolves boards like the Python build list', () => {
  const root = fakeWorkspace();
  writeFileSync(path.join(root, 'config/boards/maker.yaml'),
    'board_a:\n  name: Board A\n  target: esp32s3\n  supported_frame_grabber: [ov]\nboard_b:\n  name: Board B\n  target: esp32\n');
  writeFileSync(path.join(root, 'config/firmware/webrtc.yaml'),
    'name: WebRTC\ndescription: Camera firmware\nboards: all\nrequires_frame_grabber: true\nafter_flash:\n  text: Open it.\n  link: "#/projects/webrtc"\n');
  writeFileSync(path.join(root, 'config/firmware/blink.yaml'), 'name: Blink\ndescription: Blinks\nboards: [board_b]\n');
  const firmwares = readFirmwareCatalog(root);
  assert.deepEqual(firmwares.map(({ id, boards, afterFlash }) => ({ id, boards, afterFlash })), [
    { id: 'blink', boards: ['board_b'], afterFlash: { text: '', link: '' } },
    { id: 'webrtc', boards: ['board_a'], afterFlash: { text: 'Open it.', link: '#/projects/webrtc' } },
  ]);
  writeFileSync(path.join(root, 'config/firmware/blink.yaml'), 'name: Blink\ndescription: Blinks\nboards: [missing]\n');
  assert.throws(() => readFirmwareCatalog(root), /known board IDs/);
  writeFileSync(path.join(root, 'config/firmware/blink.yaml'), 'name: Blink\ndescription: Blinks\nboards: all\nafter_flash:\n  link: "javascript:alert(1)"\n');
  assert.throws(() => readFirmwareCatalog(root), /after_flash.link/);
});

test('the real firmware catalog offers WebRTC and Hello World for catalog boards', () => {
  const boards = readCatalog(workspace).map(board => board.id);
  const firmwares = readFirmwareCatalog(workspace);
  assert.deepEqual(firmwares.map(item => item.id).sort(), ['hello_world', 'webrtc']);
  for (const firmware of firmwares) assert.ok(firmware.boards.length && firmware.boards.every(board => boards.includes(board)), firmware.id);
});

test('the firmware manifest hides private builds unless asked', () => {
  const root = fakeWorkspace();
  fakePackage(path.join(root, 'build'), 'board_a', 'esp32s3');
  fakePackage(path.join(root, 'build'), 'board_b', 'esp32', { isPrivate: true });
  fakePackage(path.join(root, 'build'), 'not_in_catalog', 'esp32');
  assert.deepEqual(Object.keys(readFirmware(root, [{ id: 'webrtc', boards: ['board_a', 'board_b'] }], { includePrivate: false }).webrtc), ['board_a']);
  assert.deepEqual(Object.keys(readFirmware(root, [{ id: 'webrtc', boards: ['board_a', 'board_b'] }], { includePrivate: true }).webrtc), ['board_a', 'board_b']);
  assert.deepEqual(packageFiles(path.join(root, 'build/firmware/webrtc/board_a')), ['flasher_args.json', 'firmware_selection.json', 'build.json', 'firmware.bin', 'bootloader/bootloader.bin', 'webrtc.bin']);
});

test('bundle check finds missing, private, mismatched, and incomplete packages', () => {
  const root = fakeWorkspace();
  const dist = path.join(root, 'dist');
  const write = manifest => writeFileSync(path.join(dist, 'firmware/index.json'), JSON.stringify({ webrtc: manifest }));
  fakePackage(dist, 'board_a', 'esp32s3');
  write({ board_a: {} });
  assert.deepEqual(checkBundle(dist, root).problems, []);
  assert.deepEqual(checkBundle(dist, root, { allBoards: true }).problems, ['webrtc/board_b: no firmware package']);
  fakePackage(dist, 'board_b', 'esp32s3');
  write({ board_a: {}, board_b: {} });
  assert.match(checkBundle(dist, root).problems.join(), /board_b: chip esp32s3 does not match target esp32/);
  fakePackage(dist, 'board_b', 'esp32', { isPrivate: true });
  assert.match(checkBundle(dist, root).problems.join(), /private/);
  fakePackage(dist, 'board_b', 'esp32', { selectionBoard: 'board_a' });
  assert.match(checkBundle(dist, root).problems.join(), /names board_a/);
  fakePackage(dist, 'board_b', 'esp32');
  writeFileSync(path.join(dist, 'firmware/webrtc/board_b/flasher_args.json'), JSON.stringify({ flash_settings: {}, flash_files: { '0x0': 'missing.bin' }, extra_esptool_args: { chip: 'esp32' } }));
  assert.match(checkBundle(dist, root).problems.join(), /missing missing.bin/);
});
