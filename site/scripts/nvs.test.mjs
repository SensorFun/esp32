import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { parsePartitionTable, partitionTableAddress, resetNvs } from '../src/projects/flasher/nvs.mjs';

const digest = bytes => createHash('md5').update(bytes).digest();
const basic = [
  { name: 'nvs', type: 1, subtype: 2, address: 0x9000, size: 0x6000 },
  { name: 'phy_init', type: 1, subtype: 1, address: 0xf000, size: 0x1000 },
  { name: 'factory', type: 0, subtype: 0, address: 0x10000, size: 0x100000 },
];
function table(entries = basic, checksum = true) {
  const bytes = Buffer.alloc(4096, 0xff);
  entries.forEach((entry, index) => {
    const offset = index * 32;
    bytes.fill(0, offset, offset + 32);
    bytes.writeUInt16LE(0x50aa, offset);
    bytes[offset + 2] = entry.type; bytes[offset + 3] = entry.subtype;
    bytes.writeUInt32LE(entry.address, offset + 4); bytes.writeUInt32LE(entry.size, offset + 8);
    bytes.write(entry.name, offset + 12, 16); bytes.writeUInt32LE(entry.flags || 0, offset + 28);
  });
  if (checksum) {
    const offset = entries.length * 32;
    bytes.writeUInt16LE(0xebeb, offset);
    digest(bytes.subarray(0, offset)).copy(bytes, offset + 16);
  }
  return bytes;
}
function device(bytes = table()) {
  const calls = [];
  const loader = {
    calls, IS_STUB: true, ESP_ERASE_REGION: 0xd1, ERASE_REGION_TIMEOUT_PER_MB: 30000, FLASH_READ_TIMEOUT: 5000,
    detectFlashSize: async () => '4MB', flashSizeBytes: () => 0x400000,
    readFlash: async (address, size) => { calls.push(['read', address, size]); return bytes; },
    transport: { read: async () => { calls.push(['digest']); return digest(bytes); } },
    timeoutPerMb: () => 30000,
    checkCommand: async (...args) => { calls.push(['erase', ...args]); return 0; },
    flashMd5sum: async (address, size) => {
      calls.push(['verify', address, size]); return digest(Buffer.alloc(size, 0xff)).toString('hex');
    },
    eraseFlash: () => assert.fail('must never erase the full flash'),
    writeFlash: () => assert.fail('must never write firmware'),
  };
  return loader;
}
const erases = loader => loader.calls.filter(call => call[0] === 'erase');

test('reads the installed layout, erases only its NVS region and verifies erased bytes', async () => {
  const loader = device();
  const result = await resetNvs(loader, { confirm(entry) {
    assert.equal(entry.name, 'nvs'); assert.ok(Object.isFrozen(entry)); return true;
  } });
  assert.equal(result.name, 'nvs');
  assert.deepEqual(loader.calls.slice(0, 2), [['read', 0x8000, 0x1000], ['digest']]);
  const [operation] = erases(loader);
  assert.equal(operation[2], 0xd1);
  assert.deepEqual(Array.from(operation[3]), [0, 0x90, 0, 0, 0, 0x60, 0, 0]);
  assert.deepEqual(loader.calls.at(-1), ['verify', 0x9000, 0x6000]);
});

test('supports custom table placement and a project-specific NVS label', async () => {
  const loader = device(table([{ name: 'preferences', type: 1, subtype: 2, address: 0x11000, size: 0x3000 }]));
  const result = await resetNvs(loader, { tableAddress: '0x10000', confirm: () => true });
  assert.equal(result.address, 0x11000);
  assert.deepEqual(loader.calls[0], ['read', 0x10000, 0x1000]);
});

test('uses the default nvs partition without touching a second factory NVS or NVS keys', async () => {
  const loader = device(table([...basic, { name: 'factory_nvs', type: 1, subtype: 2, address: 0x200000, size: 0x1000 },
    { name: 'nvs_keys', type: 1, subtype: 4, address: 0x201000, size: 0x1000 }]));
  await resetNvs(loader, { confirm: () => true });
  assert.equal(erases(loader).length, 1);
  assert.deepEqual(loader.calls.at(-1), ['verify', 0x9000, 0x6000]);
});

test('ambiguous NVS partitions require an explicit name', async () => {
  const entries = [{ name: 'settings', type: 1, subtype: 2, address: 0x9000, size: 0x3000 },
    { name: 'preferences', type: 1, subtype: 2, address: 0xc000, size: 0x3000 }];
  const loader = device(table(entries));
  await assert.rejects(resetNvs(loader, { confirm: () => true }), /Choose an NVS partition name/);
  assert.equal(erases(loader).length, 0);
  const result = await resetNvs(loader, { partitionName: 'preferences', confirm: () => true });
  assert.equal(result.address, 0xc000);
});

test('cancellation never issues an erase or verification command', async () => {
  const loader = device();
  assert.equal(await resetNvs(loader), null);
  assert.equal(erases(loader).length, 0);
  assert.ok(!loader.calls.some(call => call[0] === 'verify'));
});

test('rejects damaged transfer digests and partition checksums without erasing', async () => {
  const loader = device();
  loader.transport.read = async () => new Uint8Array(16);
  await assert.rejects(resetNvs(loader, { confirm: () => true }), /transfer checksum/);
  assert.equal(erases(loader).length, 0);
  const bytes = table(); bytes[12] ^= 1;
  const damaged = device(bytes);
  await assert.rejects(resetNvs(damaged, { confirm: () => true }), /table checksum/);
  assert.equal(erases(damaged).length, 0);
});

test('rejects overlapping, out-of-range and misaligned layouts without erasing', async () => {
  for (const entry of [{ ...basic[0], size: 0x9000 }, { ...basic[0], address: 0x400000 },
    { ...basic[0], address: 0x9001, size: 0x1000 }, { ...basic[0], address: 0x7000, size: 0x1000 }]) {
    const loader = device(table([entry, ...basic.slice(1)]));
    await assert.rejects(resetNvs(loader, { confirm: () => true }), /Nothing erased/);
    assert.equal(erases(loader).length, 0);
  }
});

test('cannot select NVS keys, read-only or explicitly encrypted partitions', async () => {
  for (const entry of [{ ...basic[0], subtype: 4, name: 'nvs_keys' }, { ...basic[0], flags: 2 }, { ...basic[0], flags: 1 }]) {
    const loader = device(table([entry, ...basic.slice(1)]));
    await assert.rejects(resetNvs(loader, { confirm: () => true }), /Nothing erased/);
    assert.equal(erases(loader).length, 0);
  }
});

test('verification failure is reported after the erase, rather than claiming success', async () => {
  const loader = device(); loader.flashMd5sum = async () => '0'.repeat(32);
  await assert.rejects(resetNvs(loader, { confirm: () => true }), /could not be verified/);
  assert.equal(erases(loader).length, 1);
});

test('large NVS partitions are verified in bounded chunks', async () => {
  const loader = device(table([{ name: 'nvs', type: 1, subtype: 2, address: 0x9000, size: 0x21000 }]));
  await resetNvs(loader, { confirm: () => true });
  assert.deepEqual(loader.calls.filter(call => call[0] === 'verify'),
    [['verify', 0x9000, 0x10000], ['verify', 0x19000, 0x10000], ['verify', 0x29000, 0x1000]]);
});

test('supports legacy tables without MD5 records and refuses empty tables', () => {
  assert.equal(parsePartitionTable(table(basic, false), 0x8000, 0x400000)[0].name, 'nvs');
  assert.throws(() => parsePartitionTable(Buffer.alloc(4096, 0xff), 0x8000, 0x400000), /Incomplete/);
});

test('refuses unknown flash sizes, unsupported loader access and invalid addresses', async () => {
  for (const value of ['bad', '-1', '0x8001', '0x400000']) assert.throws(() => partitionTableAddress(value, 0x400000));
  const loader = device(); loader.detectFlashSize = async () => undefined;
  await assert.rejects(resetNvs(loader), /determine the flash size/);
  loader.IS_STUB = false;
  await assert.rejects(resetNvs(loader), /flash-loader access/);
  assert.equal(erases(loader).length, 0);
});
