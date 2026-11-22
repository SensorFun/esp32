// Site configuration belongs to the launcher, never to end-user form fields.
import { build, createServer, preview } from 'vite';
import { parseArgs } from 'node:util';
import { existsSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { localFirmwarePlugin } from './local-firmware.mjs';
const action = process.argv[2];
if (!['build', 'dev', 'preview'].includes(action)) throw Error('Choose build, dev, or preview');
const { values } = parseArgs({ args: process.argv.slice(3), options: {
  outDir: { type: 'string' },
  'firmware-dir': { type: 'string' },
} });
const firmwareDir = values['firmware-dir'];
if (firmwareDir !== undefined && action !== 'dev') throw Error('--firmware-dir is only available with npm run dev');
if (action === 'build') await build(values.outDir ? { build: { outDir: values.outDir } } : {});
else if (action === 'dev') {
  const plugins = [];
  if (firmwareDir !== undefined) {
    const root = path.resolve(firmwareDir);
    if (existsSync(root) && !statSync(root).isDirectory()) throw Error('--firmware-dir must point to the ESP-IDF build directory');
    plugins.push(localFirmwarePlugin(fileURLToPath(new URL('../..', import.meta.url)), root));
    console.log(`Local flasher firmware: ${root}`);
  }
  const server = await createServer({ plugins, server: { host: '127.0.0.1' } });
  await server.listen();
  server.printUrls();
}
else { const server = await preview({ preview: { host: '127.0.0.1' } }); server.printUrls(); }
