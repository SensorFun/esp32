// Browser-facing AppRTC bridge. No camera images or WebRTC keys pass here.
import http from 'node:http';
import fs from 'node:fs';
import { parseArgs } from 'node:util';
import WebSocket, { WebSocketServer } from 'ws';
import { MembershipStore } from './membership-store.mjs';
import { stunOnlyServers } from '../src/spaces/webrtc/ice.mjs';
const { values } = parseArgs({ options: { state: { type: 'string' }, config: { type: 'string' }, port: { type: 'string' }, origin: { type: 'string' } } });
if (!values.state || !values.port || !values.origin) throw Error('Required: --state PATH --port PORT --origin FRONTEND_ORIGIN [--config PATH]');
const port = Number(values.port);
const origin = new URL(values.origin).origin;
if (!Number.isInteger(port) || port < 1024 || port > 65535 || origin !== values.origin) throw Error('Invalid port or origin');
const cameras = values.config ? JSON.parse(fs.readFileSync(values.config, 'utf8')) : [];
if (!Array.isArray(cameras) || cameras.some(c => !/^[A-F0-9]{12}$/.test(c.id) || typeof c.name !== 'string')) throw Error('Invalid camera configuration');
const base = 'https://webrtc.espressif.com';
const memberships = new MembershipStore(values.state);
async function releaseRequest(method, url) {
  const response = await fetch(url, { method, headers: { Origin: base }, signal: AbortSignal.timeout(15000) });
  if (!response.ok) throw Error(`Membership cleanup returned ${response.status}`);
}
for (const entry of [...memberships.entries]) await memberships.release(entry, releaseRequest);
async function post(url, body = '') {
  const target = new URL(url);
  if (target.hostname !== 'webrtc.espressif.com' || target.protocol !== 'https:') throw Error('Unexpected signaling endpoint');
  const response = await fetch(target, { method: 'POST', body, headers: { 'Content-Type': 'text/plain', Origin: base }, signal: AbortSignal.timeout(15000) });
  if (!response.ok) throw Error(`Signaling service returned ${response.status}`);
  const text = await response.text();
  return text ? JSON.parse(text) : {};
}
const server = http.createServer((req, res) => {
  if (req.headers.origin !== origin) { res.writeHead(403).end(); return; }
  res.setHeader('Access-Control-Allow-Origin', origin);
  res.setHeader('Vary', 'Origin');
  res.setHeader('Cache-Control', 'no-store');
  if (req.method === 'OPTIONS') { res.writeHead(204).end(); return; }
  if (req.method === 'GET' && req.url === '/cameras') {
    res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(cameras)); return;
  }
  res.writeHead(404).end();
});
const wss = new WebSocketServer({ noServer: true, maxPayload: 32768 });
server.on('upgrade', (req, socket, head) => {
  const id = req.url?.slice('/session/'.length);
  if (req.headers.origin !== origin || !req.url?.startsWith('/session/') || !/^[A-F0-9]{12}$/.test(id)) { socket.destroy(); return; }
  wss.handleUpgrade(req, socket, head, ws => wss.emit('connection', ws, id));
});
const active = new Map();
const cleanups = new Set(), pending = new Set();
async function handleConnection(browser, room) {
  let params, upstream, closed = false, cleanupPromise;
  const send = message => { if (browser.readyState === WebSocket.OPEN) browser.send(JSON.stringify(message)); };
  function cleanup() {
    closed = true;
    if (cleanupPromise) return cleanupPromise;
    cleanupPromise = (async () => {
      console.log('Closing viewer membership');
      if (upstream?.readyState === WebSocket.OPEN) upstream.send(JSON.stringify({ cmd: 'send', msg: JSON.stringify({ type: 'bye' }) }));
      if (params) await memberships.release({ room, client: params.client_id }, releaseRequest);
    })().catch(error => console.warn('Membership cleanup deferred:', error.message)).finally(() => {
      upstream?.close();
      if (active.get(room) === browser) active.delete(room);
      cleanups.delete(cleanup);
    });
    return cleanupPromise;
  }
  cleanups.add(cleanup);
  if (active.has(room)) { send({ type: 'error', error: 'This camera already has a viewer.' }); browser.close(); cleanups.delete(cleanup); return; }
  active.set(room, browser);
  browser.on('close', cleanup);
  browser.on('error', cleanup);
  try {
    for (const entry of [...memberships.entries].filter(entry => entry.room === room)) await memberships.release(entry, releaseRequest);
    if (closed) return;
    const joined = await post(`${base}/join/${room}`);
    console.log('AppRTC join:', joined.result);
    if (joined.result !== 'SUCCESS') throw Error('Camera room is full or unavailable');
    params = joined.params;
    memberships.add(room, params.client_id);
    if (closed) { cleanupPromise = null; await cleanup(); return; }
    if (String(params.is_initiator) === 'true') throw Error('Camera is offline. Wait for its public signaling connection.');
    const ice = await post(params.ice_server_url);
    const wsURL = new URL(params.wss_url);
    if (wsURL.protocol !== 'wss:' || wsURL.hostname !== 'webrtc.espressif.com') throw Error('Unexpected WebSocket endpoint');
    upstream = new WebSocket(wsURL, { origin: base, maxPayload: 32768 });
    browser.on('message', async raw => {
      try {
        const message = JSON.parse(raw.toString());
        if (!['offer', 'candidate'].includes(message.type)) throw Error('Unsupported signaling message');
        console.log('Browser signaling:', message.type);
        if (upstream.readyState !== WebSocket.OPEN) throw Error('Signaling channel is disconnected');
        upstream.send(JSON.stringify({ cmd: 'send', msg: JSON.stringify(message) }));
      } catch (error) { send({ type: 'error', error: error.message }); }
    });
    upstream.on('message', raw => {
      try {
        let message = JSON.parse(raw.toString());
        if (message.msg) message = JSON.parse(message.msg);
        if (message.type === 'customized' && ['busy', 'password_not_configured', 'authentication_rate_limited'].includes(message.data)) send({ type: 'error', error: ({ busy: 'Camera is already in use.', password_not_configured: 'Set a camera password during provisioning or over USB first.', authentication_rate_limited: 'Too many login attempts; wait 30 seconds.' })[message.data] });
        else if (['offer', 'answer', 'candidate', 'bye'].includes(message.type)) { console.log('Upstream signaling:', message.type); send(message); }
      } catch { send({ type: 'error', error: 'Invalid signaling response' }); }
    });
    upstream.on('open', () => {
      if (closed) { upstream.close(); return; }
      upstream.send(JSON.stringify({ cmd: 'register', roomid: room, clientid: params.client_id }));
      const iceServers = stunOnlyServers(ice.iceServers);
      if (!iceServers.length) { send({ type: 'error', error: 'No STUN server available; TURN is disabled.' }); browser.close(); return; }
      send({ type: 'ready', iceServers });
      upstream.send(JSON.stringify({ cmd: 'send', msg: JSON.stringify({ type: 'customized', data: 'connect' }) }));
      // The browser creates a fresh offer; cached answers belong to previous sessions.
      console.log('Ignored cached messages:', (params.messages || []).length);
    });
    upstream.on('error', error => { send({ type: 'error', error: `Signaling WebSocket: ${error.message}` }); browser.close(); });
    upstream.on('close', () => { if (!closed) { send({ type: 'error', error: 'Signaling connection closed' }); browser.close(); } });
  } catch (error) { send({ type: 'error', error: error.message }); browser.close(); await cleanup(); }
}
wss.on('connection', (browser, room) => {
  const task = handleConnection(browser, room); pending.add(task); task.finally(() => pending.delete(task));
});
let shuttingDown = false;
async function shutdown() {
  if (shuttingDown) return; shuttingDown = true;
  server.close();
  for (const browser of wss.clients) browser.terminate();
  await Promise.allSettled([...pending]);
  await Promise.allSettled([...cleanups].map(cleanup => cleanup()));
  // Include entries whose earlier cleanup failed. Unreleased IDs survive restart.
  await Promise.allSettled([...memberships.entries].map(entry => memberships.release(entry, releaseRequest)));
  wss.close();
}
process.on('SIGTERM', shutdown); process.on('SIGINT', shutdown);
server.listen(port, '127.0.0.1', () => console.log(`Signaling bridge: http://localhost:${port} (frontend ${origin})`));
