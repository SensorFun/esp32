// Pure helpers for the browser flasher: build metadata, paths, port names, MD5.
const decoder = new TextDecoder();

// USB vendors of Espressif native USB and common USB-to-UART bridges.
const vendors = { 0x303a: 'Espressif USB', 0x10c4: 'Silicon Labs CP210x', 0x1a86: 'WCH CH340', 0x0403: 'FTDI', 0x067b: 'Prolific' };
const hex4 = value => value.toString(16).padStart(4, '0');

export function portLabel(info = {}) {
  const { usbVendorId: vid, usbProductId: pid } = info;
  if (vid === undefined) return 'Serial port';
  return `${vendors[vid] || 'USB serial'} (${hex4(vid)}:${hex4(pid ?? 0)})`;
}
export const isEsp32Port = (info = {}) => info.usbVendorId in vendors;

// "ESP32-S3" (esptool-js chip name) and "esp32s3" (ESP-IDF target) compare equal.
export const chipId = name => String(name).toLowerCase().replace(/[^a-z0-9]/g, '');
const chipNames = { esp32: 'ESP32', esp32s2: 'ESP32-S2', esp32s3: 'ESP32-S3', esp32c3: 'ESP32-C3', esp32c6: 'ESP32-C6', esp32h2: 'ESP32-H2', esp32p4: 'ESP32-P4' };
export const chipName = id => chipNames[id] || id.toUpperCase();

export function safePath(path) {
  if (typeof path !== 'string' || !path || path.startsWith('/') || path.includes('\\') || path.split('/').some(part => !part || part === '.' || part === '..')) {
    throw Error(`Unsafe firmware path in flasher_args.json: ${path}`);
  }
  return path;
}

const flashModes = ['dio', 'qio', 'dout', 'qout', 'keep'];
const flashFreqs = ['80m', '60m', '48m', '40m', '30m', '26m', '24m', '20m', '16m', '15m', '12m', 'keep'];
const flashSizes = ['256KB', '512KB', '1MB', '2MB', '4MB', '8MB', '16MB', '32MB', '64MB', '128MB', 'keep'];
const sizeBytes = size => size === 'keep' ? Infinity : parseInt(size, 10) * (size.endsWith('MB') ? 1 << 20 : 1 << 10);

// Reads ESP-IDF's build/<project>/<board>/flasher_args.json, as tools/flash.py does.
export function parseFlasherArgs(bytes) {
  let args;
  try { args = JSON.parse(typeof bytes === 'string' ? bytes : decoder.decode(bytes)); } catch { throw Error('flasher_args.json is not valid JSON'); }
  const extra = args?.extra_esptool_args, settings = args?.flash_settings ?? {};
  if (!extra || typeof extra.chip !== 'string' || !args.flash_files || typeof args.flash_files !== 'object') throw Error('flasher_args.json is missing flash files or chip');
  const flashMode = settings.flash_mode ?? 'keep', flashFreq = settings.flash_freq ?? 'keep', flashSize = settings.flash_size ?? 'keep';
  if (!flashModes.includes(flashMode) || !flashFreqs.includes(flashFreq) || !flashSizes.includes(flashSize)) throw Error('Unsupported flash settings in flasher_args.json');
  const files = Object.entries(args.flash_files).map(([offset, path]) => {
    if (!/^0x[0-9a-f]+$/i.test(offset)) throw Error(`Invalid flash offset: ${offset}`);
    return { address: parseInt(offset, 16), path: safePath(path) };
  }).sort((a, b) => a.address - b.address);
  if (!files.length) throw Error('flasher_args.json lists no files to flash');
  return {
    chip: chipId(extra.chip), flashMode, flashFreq, flashSize, files,
    before: String(extra.before ?? 'default-reset').replace(/-/g, '_'),
    after: String(extra.after ?? 'hard-reset').replace(/-/g, '_'),
  };
}

// Checks file sizes against each other and the flash size once file data is known.
export function checkLayout(plan, sizes) {
  const limit = sizeBytes(plan.flashSize);
  plan.files.forEach((file, index) => {
    const end = file.address + sizes[index];
    if (end > limit) throw Error(`${file.path} does not fit in ${plan.flashSize} flash`);
    const next = plan.files[index + 1];
    if (next && end > next.address) throw Error(`${file.path} overlaps ${next.path}`);
  });
}

// Finds folders that hold flasher_args.json among relative file paths.
export function firmwareRoots(paths) {
  return paths.filter(path => path === 'flasher_args.json' || path.endsWith('/flasher_args.json'))
    .map(path => path.slice(0, -'flasher_args.json'.length).replace(/\/$/, '')).sort();
}

// MD5 for esptool-js's post-write verification; Web Crypto has no MD5.
const shifts = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const table = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);
export function md5(data) {
  const length = data.length, padded = new Uint8Array(((length + 72) >>> 6) << 6);
  padded.set(data); padded[length] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, (length * 8) >>> 0, true);
  view.setUint32(padded.length - 4, Math.floor(length / 2 ** 29), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const words = new Uint32Array(16);
  for (let offset = 0; offset < padded.length; offset += 64) {
    for (let i = 0; i < 16; i++) words[i] = view.getUint32(offset + i * 4, true);
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      const round = i >> 4;
      let f, g;
      if (round === 0) { f = (b & c) | (~b & d); g = i; }
      else if (round === 1) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (round === 2) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~d); g = (7 * i) & 15; }
      const sum = (a + f + table[i] + words[g]) >>> 0, shift = shifts[round * 4 + (i & 3)];
      a = d; d = c; c = b;
      b = (b + ((sum << shift) | (sum >>> (32 - shift)))) >>> 0;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
  }
  return [a0, b0, c0, d0].map(word => Array.from({ length: 4 }, (_, i) => ((word >>> (i * 8)) & 255).toString(16).padStart(2, '0')).join('')).join('');
}
