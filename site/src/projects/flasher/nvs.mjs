import { md5 } from './plan.mjs';

const sector = 0x1000, tableLength = 0xc00;
const hex = bytes => Array.from(bytes, value => value.toString(16).padStart(2, '0')).join('');

export function partitionTableAddress(text, flashSize) {
  if (!/^(0x[0-9a-f]+|[0-9]+)$/i.test(String(text))) throw Error('Enter a valid partition table address.');
  const address = Number(text);
  if (!Number.isSafeInteger(address) || address < sector || address % sector || address + sector > flashSize) {
    throw Error('The partition table address must be sector-aligned and inside the flash.');
  }
  return address;
}

export function parsePartitionTable(bytes, address, flashSize) {
  if (!(bytes instanceof Uint8Array) || bytes.length !== sector) throw Error('Incomplete partition table read. Nothing erased.');
  partitionTableAddress(address, flashSize);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const entries = [];
  let ended = false;
  for (let offset = 0; offset < tableLength; offset += 32) {
    const row = bytes.subarray(offset, offset + 32);
    if (row.every(value => value === 0xff)) {
      if (!bytes.subarray(offset).every(value => value === 0xff)) throw Error('Invalid partition table padding. Nothing erased.');
      ended = true; break;
    }
    const magic = view.getUint16(offset, true);
    if (magic === 0xebeb) {
      if (!row.subarray(2, 16).every(value => value === 0xff) ||
          hex(row.subarray(16)) !== md5(bytes.subarray(0, offset)) ||
          !bytes.subarray(offset + 32).every(value => value === 0xff)) {
        throw Error('Partition table checksum is invalid. Nothing erased.');
      }
      ended = true; break;
    }
    if (magic !== 0x50aa) throw Error('No valid ESP-IDF partition table found. Check its address in Advanced options. Nothing erased.');
    const label = row.subarray(12, 28), end = label.indexOf(0);
    const name = new TextDecoder('utf-8', { fatal: true }).decode(end < 0 ? label : label.subarray(0, end));
    const entry = { name, type: row[2], subtype: row[3], address: view.getUint32(offset + 4, true),
      size: view.getUint32(offset + 8, true), flags: view.getUint32(offset + 28, true) };
    if (!name || /[\x00-\x1f\x7f]/.test(name) || entry.type === 0xff || !entry.size ||
        entry.address + entry.size > flashSize || entries.some(other => other.name === name ||
          entry.address < other.address + other.size && other.address < entry.address + entry.size)) {
      throw Error('Invalid or overlapping partition layout. Nothing erased.');
    }
    entries.push(entry);
  }
  if (!ended || !entries.length) throw Error('Incomplete partition table. Nothing erased.');
  return entries;
}

// The pinned esptool-js readFlash leaves the stub's trailing digest unread.
// Consume and verify it before issuing another bootloader command.
async function readTable(loader, address) {
  const data = await loader.readFlash(address, sector);
  const digest = await loader.transport.read(loader.FLASH_READ_TIMEOUT);
  if (!(data instanceof Uint8Array) || data.length !== sector ||
      !(digest instanceof Uint8Array) || digest.length !== 16 || hex(digest) !== md5(data)) {
    throw Error('Partition table transfer checksum failed. Nothing erased.');
  }
  return data;
}

export async function resetNvs(loader, { tableAddress = '0x8000', partitionName = '',
  confirm = () => false, status = () => {} } = {}) {
  if (!loader.IS_STUB || loader.secureDownloadMode) throw Error('This device does not allow the required flash-loader access. Nothing erased.');
  status('Reading the installed partition table…');
  const detected = await loader.detectFlashSize();
  if (!detected) throw Error('Could not determine the flash size. Nothing erased.');
  const flashSize = loader.flashSizeBytes(detected);
  if (!Number.isSafeInteger(flashSize) || flashSize < sector * 2) throw Error('Could not determine the flash size. Nothing erased.');
  const address = partitionTableAddress(tableAddress, flashSize);
  const entries = parsePartitionTable(await readTable(loader, address), address, flashSize);
  const candidates = entries.filter(entry => entry.type === 1 && entry.subtype === 2);
  const name = partitionName.trim();
  let selected = candidates.find(entry => entry.name === (name || 'nvs'));
  if (!selected && !name && candidates.length === 1) selected = candidates[0];
  if (!selected) {
    if (!candidates.length) throw Error('No NVS settings partition found. Nothing erased.');
    throw Error(`Choose an NVS partition name in Advanced options: ${candidates.map(entry => entry.name).join(', ')}. Nothing erased.`);
  }
  if (selected.address < address + sector || selected.address % sector || selected.size % sector || selected.flags & 3) {
    throw Error('The NVS partition is encrypted, read-only, misaligned or overlaps the boot area. Nothing erased.');
  }
  if (await confirm(Object.freeze({ ...selected })) !== true) return null;
  status(`Erasing NVS partition “${selected.name}”…`);
  const packet = new Uint8Array(8), view = new DataView(packet.buffer);
  view.setUint32(0, selected.address, true); view.setUint32(4, selected.size, true);
  await loader.checkCommand('erase NVS partition', loader.ESP_ERASE_REGION, packet, 0, 0,
    loader.timeoutPerMb(loader.ERASE_REGION_TIMEOUT_PER_MB, selected.size));
  status('Verifying the NVS erase…');
  // Check bounded chunks without reading settings or allocating a partition-sized buffer.
  const hashes = new Map();
  for (let offset = 0; offset < selected.size; offset += 0x10000) {
    const size = Math.min(0x10000, selected.size - offset);
    if (!hashes.has(size)) hashes.set(size, md5(new Uint8Array(size).fill(0xff)));
    if ((await loader.flashMd5sum(selected.address + offset, size)).toLowerCase() !== hashes.get(size)) {
      throw Error('The NVS erase could not be verified. Reconnect the board and try again.');
    }
  }
  return selected;
}
