import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';
import { boardsPlugin } from './scripts/boards-plugin.mjs';
import { routesPlugin } from './scripts/routes-plugin.mjs';

const site = path.dirname(fileURLToPath(import.meta.url));
const workspace = path.resolve(site, '..');

export default defineConfig({
  // Path routes need absolute asset URLs. The site is served at the domain
  // root; `npm run build -- --base /folder/` serves it from a subfolder.
  base: '/',
  esbuild: { jsx: 'automatic' },
  plugins: [boardsPlugin(workspace), routesPlugin()],
  // Board images live beside the board catalog, outside the site folder.
  server: { fs: { allow: [site, path.join(workspace, 'config/boards/images')] } },
  build: { outDir: 'dist', emptyOutDir: true },
  // The flasher loads esptool-js with a dynamic import. Bundle it at startup so
  // the dev server never re-optimizes dependencies (and reloads) mid-flash.
  optimizeDeps: { include: ['esptool-js'] },
});
