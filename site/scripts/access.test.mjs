import test from 'node:test';
import assert from 'node:assert/strict';
import { webcrypto, pbkdf2Sync, createHmac, createHash } from 'node:crypto';
import { normalizeMac, roomForMac, deriveCameraAccess, createProof, verifyServer, fingerprint } from '../src/spaces/webrtc/access.mjs';
globalThis.crypto = webcrypto;
const hmac = (key, value) => createHmac('sha256', key).update(value).digest();
const digest = value => createHash('sha256').update(value).digest();
const challenge = { algorithm: 'camera-scram-sha256-v2', salt: '00'.repeat(16), nonce: '01'.repeat(32), iterations: 210000 };
const offer = `v=0\r\na=fingerprint:sha-256 ${Array(32).fill('AB').join(':')}\r\n`;
const answer = offer.replaceAll('AB', 'CD');
// Synthetic test credentials and locally administered MACs, never user data.
const password = 'synthetic-test-vector';
const camera = '020000000001';
test('MAC normalization rejects malformed identifiers', () => {
  for (const mac of ['02:00:00:00:00:01', '02-00-00-00-00-01', camera]) assert.equal(roomForMac(mac), camera);
  assert.equal(roomForMac('02:ab:cd:ef:00:01'), '02ABCDEF0001');
  for (const mac of ['02000000000', '02:00-00:00:00:01', '../room', 'camera-020000000001']) assert.throws(() => normalizeMac(mac));
});
test('secret room matches independent PBKDF2/HMAC calculation and stays stable', async () => {
  const access = await deriveCameraAccess(camera, password);
  const root = pbkdf2Sync(password, digest(`camera-kdf-v2\n${camera}`), 210000, 32, 'sha256');
  assert.equal(access.room, hmac(root, `camera-room-v1\n${camera}`).subarray(0, 16).toString('hex').toUpperCase());
  assert.match(access.room, /^[A-F0-9]{32}$/);
  assert.equal((await deriveCameraAccess('02:00:00:00:00:01', password)).room, access.room);
  assert.notEqual((await deriveCameraAccess(camera, 'different-test-vector')).room, access.room);
  assert.notEqual((await deriveCameraAccess('020000000002', password)).room, access.room);
  assert.equal(access.key.extractable, false);
  await assert.rejects(deriveCameraAccess(camera, 'short'));
});
test('domain-separated authentication matches independent calculation; nonces and certificates prevent replay', async () => {
  const access = await deriveCameraAccess(camera, password);
  const result = await createProof(password, camera, challenge, offer, answer, access);
  const text = ['camera-scram-v2', camera, challenge.salt, 210000, challenge.nonce, 'ab'.repeat(32), 'cd'.repeat(32)].join('\n');
  const root = pbkdf2Sync(password, digest(`camera-kdf-v2\n${camera}`), 210000, 32, 'sha256');
  const salted = hmac(root, `camera-auth-v2\n${challenge.salt}`);
  const client = hmac(salted, 'Client Key'), stored = digest(client), signature = hmac(stored, text);
  assert.equal(result.proof, Buffer.from(client.map((value, index) => value ^ signature[index])).toString('hex'));
  assert.equal(result.expected, hmac(hmac(salted, 'Server Key'), text).toString('hex'));
  assert(verifyServer(result.expected, result.expected));
  assert(!verifyServer('00'.repeat(32), result.expected));
  const wrong = await createProof('incorrect-test-vector', camera, challenge, offer, answer);
  assert.notEqual(wrong.proof, result.proof);
  for (const [changed, changedOffer, changedAnswer] of [[{ ...challenge, nonce: '02'.repeat(32) }, offer, answer], [challenge, answer, answer], [challenge, offer, offer]]) {
    assert.notEqual((await createProof(password, camera, changed, changedOffer, changedAnswer, access)).proof, result.proof);
  }
  await assert.rejects(createProof(password, camera, { ...challenge, iterations: 1 }, offer, answer, access));
  assert.throws(() => fingerprint('v=0\r\n'));
  assert.throws(() => fingerprint(offer + answer));
  assert.throws(() => fingerprint(offer + 'a=fingerprint:sha-1 AB\r\n'));
});
