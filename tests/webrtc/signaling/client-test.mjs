// Integration test against the real Docker image, using disposable credentials.
// Run `npm ci --prefix site`, build the image, then `node --test tests/webrtc/signaling/run.mjs`.
import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, webcrypto } from 'node:crypto';
import { createRequire } from 'node:module';
import { setTimeout as delay } from 'node:timers/promises';
import { Signaling } from '../../../site/src/projects/webrtc/signaling.mjs';
const require = createRequire(new URL('../../../site/package.json', import.meta.url));
const WebSocket = require('ws');
globalThis.WebSocket = WebSocket;
globalThis.crypto ??= webcrypto;
const secret = process.env.SIGNALING_API_SECRET;
const viewerEndpoint = process.env.TEST_VIEWER_URL;
const cameraEndpoint = process.env.TEST_CAMERA_URL;
const sockets = new Set();
after(() => { for (const socket of sockets) socket.close(); });

class Peer {
  async open(target) {
    this.ws = new WebSocket(target); sockets.add(this.ws);
    this.queue = []; this.waiters = [];
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
        this.waiters.splice(this.waiters.indexOf(waiter), 1); reject(Error('Timed out waiting for the signaling server'));
      }, 5000) };
      this.waiters.push(waiter);
    });
  }
  join(room, role, key, preservePeer = false) {
    const body = { type: 'join', room, preserve_peer: preservePeer };
    if (role === 'camera') body.secret = key ?? secret;
    this.ws.send(JSON.stringify(body));
    return this.next(value => value.type === 'joined' || value.type === 'error');
  }
  signal(message) { this.ws.send(JSON.stringify(message)); }
  received(type) { return this.next(value => value.type === type); }
  close() { this.ws.close(); }
}
const roomId = () => randomBytes(16).toString('hex').toUpperCase();

test('API secret rejects unauthorized camera join', async () => {
  const camera = await new Peer().open(cameraEndpoint);
  const result = await camera.join(roomId(), 'camera', randomBytes(32).toString('hex'));
  assert.equal(result.type, 'error');
  camera.close();
});

test('camera without any secret is rejected and disconnected', async () => {
  const camera = await new Peer().open(cameraEndpoint);
  const closed = new Promise(resolve => camera.ws.once('close', resolve));
  camera.ws.send(JSON.stringify({ type: 'join', room: roomId() }));
  await closed;
});

test('non-ASCII camera credentials are rejected cleanly', async () => {
  const camera = await new Peer().open(cameraEndpoint);
  assert.equal((await camera.join(roomId(), 'camera', '\u00e9'.repeat(32))).type, 'error');
  camera.close();
});

test('failed HTTP upgrades do not exhaust connection slots', async () => {
  const url = viewerEndpoint.replace(/^ws:/, 'http:');
  for (let index = 0; index < 260; index++) {
    const response = await fetch(url);
    assert.equal(response.status, 400);
    await response.arrayBuffer();
  }
  const camera = await new Peer().open(cameraEndpoint);
  assert.equal((await camera.join(roomId(), 'camera')).type, 'joined');
  camera.close();
});

test('offers exceeding the firmware limit are rejected before relay', async () => {
  for (const sdp of ['x'.repeat(16385), '\u00e9'.repeat(8193), '\ud800']) {
    const room = roomId();
    const camera = await new Peer().open(cameraEndpoint);
    assert.equal((await camera.join(room, 'camera')).type, 'joined');
    const viewer = await new Peer().open(viewerEndpoint);
    assert.equal((await viewer.join(room, 'viewer')).type, 'joined');
    await camera.received('customized');
    const closed = new Promise(resolve => viewer.ws.once('close', resolve));
    viewer.signal({ type: 'offer', sdp });
    await closed;
    await camera.received('bye');
    assert(!camera.queue.some(message => message.type === 'offer'));
    camera.close();
  }
});

test('signals sent by the wrong role close the connection', async () => {
  const room = roomId();
  const camera = await new Peer().open(cameraEndpoint);
  assert.equal((await camera.join(room, 'camera')).type, 'joined');
  const closed = new Promise(resolve => camera.ws.once('close', resolve));
  camera.signal({ type: 'offer', sdp: 'synthetic' });
  await closed;
});

test('real browser adapter exchanges opaque SDP and ICE; rooms isolate peers and release membership', async () => {
  const room = roomId();
  const camera = await new Peer().open(cameraEndpoint);
  assert.equal((await camera.join(room, 'camera')).type, 'joined');
  const browser = new Signaling(room, viewerEndpoint);
  sockets.add(browser.socket);
  const browserRequests = [];
  const originalSend = browser.socket.send.bind(browser.socket);
  browser.socket.send = data => { browserRequests.push(JSON.parse(data)); return originalSend(data); };
  const messages = [];
  let failure;
  browser.onerror = error => { failure = error; };
  browser.onmessage = event => messages.push(JSON.parse(event.data));
  assert.equal((await camera.received('customized')).data, 'connect');
  camera.signal({ type: 'customized', data: JSON.stringify({ type: 'ready', iceServers: [{ urls: 'stun:stun.example.test:3478' }] }) });
  for (let count = 0; messages.length < 1 && count < 50; count++) await delay(20);
  assert.equal(messages[0].type, 'ready');
  browser.send(JSON.stringify({ type: 'offer', sdp: 'v=0\r\nopaque-synthetic-offer' }));
  assert.equal((await camera.received('offer')).sdp, 'v=0\r\nopaque-synthetic-offer');
  camera.signal({ type: 'answer', sdp: 'v=0\r\nopaque-synthetic-answer' });
  camera.signal({ type: 'candidate', candidate: 'synthetic-ice-candidate' });
  for (let count = 0; messages.length < 3 && count < 50; count++) await delay(20);
  assert.deepEqual(messages.slice(1).map(message => message.type), ['answer', 'candidate']);
  assert.equal(failure, undefined);
  assert(browserRequests.every(request => !('secret' in request) && !('apisecret' in request) && !('token' in request)));
  const unknown = await new Peer().open(viewerEndpoint);
  assert.match((await unknown.join(roomId(), 'viewer')).message, /Camera not found/);
  unknown.close();
  const duplicate = await new Peer().open(viewerEndpoint);
  assert.match((await duplicate.join(room, 'viewer')).message, /already connected/);
  duplicate.close();
  const isolated = await new Peer().open(viewerEndpoint);
  assert.match((await isolated.join(roomId(), 'viewer')).message, /Camera not found/);
  isolated.close();
  browser.close();
  assert.equal((await camera.received('bye')).type, 'bye');
  const replacement = await new Peer().open(viewerEndpoint);
  assert.equal((await replacement.join(room, 'viewer')).type, 'joined');
  await camera.received('customized');
  replacement.close(); camera.close();
});

test('browser rejects insecure or credential-bearing remote endpoints', () => {
  for (const url of ['ws://signal.example.test', 'wss://user:password@signal.example.test', 'wss://signal.example.test?secret=value']) {
    assert.throws(() => new Signaling(roomId(), url), /secure wss/);
  }
});

test('a dropped signaling socket differs from an explicit disconnect', async () => {
  const room = roomId();
  const camera = await new Peer().open(cameraEndpoint);
  assert.equal((await camera.join(room, 'camera', undefined, true)).type, 'joined');
  const legacyViewer = await new Peer().open(viewerEndpoint);
  assert.equal((await legacyViewer.join(room, 'viewer')).type, 'joined');
  await camera.received('customized');
  legacyViewer.close();
  await camera.received('bye');

  const firstViewer = await new Peer().open(viewerEndpoint);
  assert.equal((await firstViewer.join(room, 'viewer', undefined, true)).type, 'joined');
  await camera.received('customized');
  firstViewer.ws.terminate();
  await camera.received('signaling_lost');
  assert(!camera.queue.some(message => message.type === 'bye'));

  const browser = new Signaling(room, viewerEndpoint);
  sockets.add(browser.socket);
  await camera.received('customized');
  for (let count = 0; !browser.joined && count < 50; count++) await delay(20);
  assert(browser.joined);
  browser.close({ notifyPeer: true });
  await camera.received('bye');
  await camera.received('signaling_lost');

  const nextViewer = new Signaling(room, viewerEndpoint);
  sockets.add(nextViewer.socket);
  const notice = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(Error('Missing camera signaling loss notice')), 5000);
    nextViewer.onmessage = event => { clearTimeout(timer); resolve(JSON.parse(event.data)); };
  });
  await camera.received('customized');
  camera.ws.terminate();
  assert.equal((await notice).type, 'signaling_lost');
  nextViewer.close();
});
