// Site configuration belongs to the launcher, never to end-user form fields.
import { build, createServer, preview } from 'vite';
import { parseArgs } from 'node:util';
const action = process.argv[2];
if (!['build', 'dev', 'preview'].includes(action)) throw Error('Choose build, dev, or preview');
const { values } = parseArgs({ args: process.argv.slice(3), options: {
  outDir: { type: 'string' },
} });
if (action === 'build') await build(values.outDir ? { build: { outDir: values.outDir } } : {});
else if (action === 'dev') { const server = await createServer({ server: { host: '127.0.0.1' } }); await server.listen(); server.printUrls(); }
else { const server = await preview({ preview: { host: '127.0.0.1' } }); server.printUrls(); }
