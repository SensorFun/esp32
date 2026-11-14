// Site configuration belongs to the launcher, never to end-user form fields.
import { build, createServer, preview } from 'vite';
const action = process.argv[2];
if (!['build', 'dev', 'preview'].includes(action)) throw Error('Choose build, dev, or preview');
if (action !== 'preview') {
  const value = process.env.SIGNALING_URL;
  if (!value) throw Error('Set SIGNALING_URL to your public wss:// hostname followed by /viewer');
  const url = new URL(value);
  if (url.protocol !== 'wss:' || url.username || url.password || url.search || url.hash || url.pathname !== '/viewer') {
    throw Error('SIGNALING_URL must be a wss:// URL ending in /viewer, without credentials, queries, or fragments');
  }
  process.env.VITE_SIGNALING_URL = url.href;
}
if (action === 'build') await build();
else if (action === 'dev') { const server = await createServer({ server: { host: '127.0.0.1' } }); await server.listen(); server.printUrls(); }
else { const server = await preview({ preview: { host: '127.0.0.1' } }); server.printUrls(); }
