import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';
import { boardsPlugin } from './scripts/boards-plugin.mjs';

const site = path.dirname(fileURLToPath(import.meta.url));
const workspace = path.resolve(site, '..');

export default defineConfig({
  // Relative assets and hash routes work at / as well as /repository-name/.
  base: './',
  esbuild: { jsx: 'automatic' },
  plugins: [boardsPlugin(workspace)],
  // Board images live beside the board catalog, outside the site folder.
  server: { fs: { allow: [site, path.join(workspace, 'config/boards/images')] } },
  build: { outDir: 'dist', emptyOutDir: true },
});
