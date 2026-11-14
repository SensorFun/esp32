import { test } from 'node:test';
import assert from 'node:assert/strict';
import { stunOnlyServers } from '../src/spaces/webrtc/ice.mjs';

test('STUN-only policy removes relays and credentials from mixed service configuration', () => {
  const input = [
    { urls: ['stun:stun.example.test:3478', 'turn:relay.example.test:3478', 'turns:relay.example.test:5349'], username: 'temporary', credential: 'temporary' },
    { urls: 'turn:relay.example.test:3478' },
    { urls: 'stun:another.example.test:3478', username: 'unused' },
    { urls: [null, 12, 'https://example.test', 'stun:bad address'] }, null,
  ];
  assert.deepEqual(stunOnlyServers(input), [
    { urls: ['stun:stun.example.test:3478'] }, { urls: ['stun:another.example.test:3478'] },
  ]);
  assert.deepEqual(stunOnlyServers([{ urls: 'turns:relay.example.test' }]), []);
  assert.throws(() => stunOnlyServers(null), TypeError);
});
