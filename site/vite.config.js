import { defineConfig } from 'vite';

export default defineConfig({
  // Relative assets and hash routes work at / as well as /repository-name/.
  base: './',
  esbuild: { jsx: 'automatic' },
  build: { outDir: 'dist', emptyOutDir: true },
});
