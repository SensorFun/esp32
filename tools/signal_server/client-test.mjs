// Integration test against the real Docker image, using disposable credentials.
// Run `npm ci --prefix site`, build the image, then `node --test tools/signal_server/test.mjs`.
import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, randomUUID, webcrypto } from 'node:crypto';
import { createRequire } from 'node:module';
import { setTimeout as delay } from 'node:timers/promises';
import { JanusSignaling } from '../../site/src/spaces/webrtc/janus-signaling.mjs';
const require = createRequire(new URL('../../site/package.json', import.meta.url));
const WebSocket = require('ws');
globalThis.WebSocket = WebSocket;
globalThis.crypto ??= webcrypto;
const secret = process.env.JANUS_API_SECRET;
const endpoint = process.env.TEST_JANUS_URL;
const viewerEndpoint = process.env.TEST_VIEWER_URL;
const cameraEndpoint = process.env.TEST_CAMERA_URL;
const sockets = new Set();
after(() => { for (const socket of sockets) socket.close(); });

class Peer {
  async open(key = secret, target = endpoint) {
    this.secret = key; this.queue = []; this.waiters = [];
    this.ws = new WebSocket(target, 'janus-protocol'); sockets.add(this.ws);
    this.ws.on('message', bytes => {
      const value = JSON.parse(bytes);
      const index = this.waiters.findIndex(waiter => waiter.match(value));
      if (index >= 0) { const [waiter] = this.waiters.splice(index, 1); clearTimeout(waiter.timer); waiter.resolve(value); }
      else this.queue.push(value);
    });
    await new Promise((resolve, reject) => { this.ws.once('open', resolve); this.ws.once('error', reject); });
    return this;
  }
  next(match) {
    const index = this.queue.findIndex(match);
    if (index >= 0) return Promise.resolve(this.queue.splice(index, 1)[0]);
    return new Promise((resolve, reject) => {
      const waiter = { match, resolve, timer: setTimeout(() => {
        this.waiters.splice(this.waiters.indexOf(waiter), 1); reject(Error('Timed out waiting for Janus'));
      }, 5000) };
      this.waiters.push(waiter);
    });
  }
  request(janus, extra = {}) {
    const transaction = randomUUID();
    this.ws.send(JSON.stringify({ janus, transaction, ...(this.secret ? { apisecret: this.secret } : {}), session_id: this.session, handle_id: this.handle, ...extra }));
    return this.next(value => value.transaction === transaction && value.janus !== 'ack');
  }
  async join(room, role) {
    this.session = (await this.request('create')).data.id;
    this.handle = (await this.request('attach', { plugin: 'janus.plugin.signaling' })).data.id;
    return this.request('message', { body: { request: 'join', room, role } });
  }
  signal(message) { return this.request('message', { body: { request: 'send', message } }); }
  received(type) { return this.next(value => value.plugindata?.data?.message?.type === type); }
  async close() { await this.request('destroy'); this.ws.close(); }
}
const roomId = () => randomBytes(16).toString('hex').toUpperCase();

test('API secret rejects unauthorized session creation', async () => {
  const peer = await new Peer().open(randomBytes(32).toString('hex'));
  const result = await peer.request('create');
  assert.equal(result.janus, 'error'); assert.equal(result.error.code, 403);
  peer.ws.close();
});

test('real browser adapter exchanges opaque SDP and ICE; rooms isolate peers and release membership', async () => {
  const room = roomId();
  const camera = await new Peer().open(secret, cameraEndpoint);
  assert.equal((await camera.join(room, 'camera')).plugindata.data.event, 'joined');
  const browser = new JanusSignaling(room, viewerEndpoint);
  sockets.add(browser.socket);
  const browserRequests = [];
  const originalSend = browser.socket.send.bind(browser.socket);
  browser.socket.send = data => { browserRequests.push(JSON.parse(data)); return originalSend(data); };
  const messages = [];
  let failure;
  browser.onerror = error => { failure = error; };
  browser.onmessage = event => messages.push(JSON.parse(event.data));
  assert.equal((await camera.received('customized')).plugindata.data.message.data, 'connect');
  await camera.signal({ type: 'customized', data: JSON.stringify({ type: 'ready', iceServers: [{ urls: 'stun:stun.example.test:3478' }] }) });
  for (let count = 0; messages.length < 1 && count < 50; count++) await delay(20);
  assert.equal(messages[0].type, 'ready');
  browser.send(JSON.stringify({ type: 'offer', sdp: 'v=0\r\nopaque-synthetic-offer' }));
  assert.equal((await camera.received('offer')).plugindata.data.message.sdp, 'v=0\r\nopaque-synthetic-offer');
  await camera.signal({ type: 'answer', sdp: 'v=0\r\nopaque-synthetic-answer' });
  await camera.signal({ type: 'candidate', candidate: 'synthetic-ice-candidate' });
  for (let count = 0; messages.length < 3 && count < 50; count++) await delay(20);
  assert.deepEqual(messages.slice(1).map(message => message.type), ['answer', 'candidate']);
  assert.equal(failure, undefined);
  assert(browserRequests.every(request => !('apisecret' in request) && !('token' in request)));
  const unknown = await new Peer().open(null, viewerEndpoint);
  assert.match((await unknown.join(roomId(), 'viewer')).plugindata.data.error, /Camera not found/);
  unknown.ws.close();
  const duplicate = await new Peer().open();
  assert.match((await duplicate.join(room, 'viewer')).plugindata.data.error, /already connected/);
  await duplicate.close();
  const isolated = await new Peer().open();
  assert.match((await isolated.join(roomId(), 'viewer')).plugindata.data.error, /Camera not found/);
  await isolated.close();
  assert.match((await camera.signal({ type: 'offer', sdp: 'synthetic' })).plugindata.data.error, /Invalid signal/);
  const jsep = await camera.request('message', { body: { request: 'send', message: { type: 'bye' } }, jsep: { type: 'offer', sdp: 'synthetic' } });
  assert(jsep.janus === 'error' || /JSEP/.test(jsep.plugindata?.data?.error));
  browser.close();
  await camera.received('bye');
  const replacement = await new Peer().open();
  assert.equal((await replacement.join(room, 'viewer')).plugindata.data.event, 'joined');
  await camera.received('customized');
  await replacement.close(); await camera.close();
});

test('public gateway cannot register cameras or access arbitrary Janus operations', async () => {
  const attacker = await new Peer().open(null, viewerEndpoint);
  attacker.session = (await attacker.request('create')).data.id;
  attacker.handle = (await attacker.request('attach', { plugin: 'janus.plugin.signaling' })).data.id;
  const closed = new Promise(resolve => attacker.ws.once('close', resolve));
  attacker.ws.send(JSON.stringify({ janus: 'message', transaction: randomUUID(), body: { request: 'join', room: roomId(), role: 'camera' } }));
  await closed;
  const camera = await new Peer().open(randomBytes(32).toString('hex'), cameraEndpoint);
  assert.equal((await camera.request('create')).error.code, 403);
  camera.ws.close();
});

test('browser rejects insecure or credential-bearing remote endpoints', () => {
  for (const url of ['ws://signal.example.test', 'wss://user:password@signal.example.test', 'wss://signal.example.test?secret=value']) {
    assert.throws(() => new JanusSignaling(roomId(), url), /secure wss/);
  }
});
