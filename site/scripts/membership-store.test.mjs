import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { MembershipStore } from './membership-store.mjs';
test('membership survives restart and failed cleanup; removed only after both endpoints succeed', async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'camera-membership-'));
  try {
    const filename = path.join(directory, 'state.json');
    let store = new MembershipStore(filename);
    store.add('020000000001', '12345678');
    store = new MembershipStore(filename);
    assert.equal(store.entries.length, 1);
    for (const failedMethod of ['POST', 'DELETE']) {
      await assert.rejects(store.release(store.entries[0], async method => { if (method === failedMethod) throw Error('network unavailable'); }));
      assert.equal(new MembershipStore(filename).entries.length, 1);
    }
    const methods = [];
    await store.release(store.entries[0], async method => methods.push(method));
    assert.deepEqual(methods, ['POST', 'DELETE']);
    assert.equal(new MembershipStore(filename).entries.length, 0);
  } finally { fs.rmSync(directory, { recursive: true }); }
});
