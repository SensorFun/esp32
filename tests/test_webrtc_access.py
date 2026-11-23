"""Check the firmware's authentication serialization independently of the browser."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AccessProtocolTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'Native C++ compiler required')
    def test_authentication_transcript(self):
        source = r'''
#include "auth_protocol.h"
#include <cassert>
int main() {
    using namespace access_protocol;
    uint8_t bytes[2]{};
    assert(unhex("aB01", bytes, 2));
    assert(hex(bytes, 2) == "ab01");
    assert(!unhex("ab0z", bytes, 2));
    assert(!unhex("ab", bytes, 2));
    std::string cert;
    for (int i = 0; i < 32; ++i) cert += i ? ":AB" : "AB";
    const std::string sdp = "v=0\r\na=fingerprint:sha-256 " + cert + "\r\n";
    std::string fp;
    for (int i = 0; i < 32; ++i) fp += "ab";
    assert(fingerprint(sdp) == fp);
    assert(fingerprint(sdp + sdp).empty());
    assert(fingerprint(sdp + "a=fingerprint:sha-1 AB\r\n").empty());
    assert(fingerprint("x" + sdp.substr(5)).empty());
    assert(fingerprint("v=0\r\na=fingerprint:sha-256 AB\r\n").empty());
    assert(fingerprint("v=0\r\na=fingerprint:sha-1 " + cert).empty());
    assert(transcript("020000000001", "salt", 210000, "nonce", "offer", "answer") ==
        "camera-scram-v2\n020000000001\nsalt\n210000\nnonce\noffer\nanswer");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(source)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'projects/webrtc/main'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    @unittest.skipUnless(shutil.which('node'), 'Node.js required for browser logic tests')
    def test_local_viewer_proof_matches_website(self):
        # The board's own viewer inlines the password proof; it must stay
        # identical to the website's remote viewer, which the firmware verifies.
        html = (ROOT / 'projects/webrtc/main/viewer.html').read_text()
        start, end = html.index('let pc,channel'), html.index('function verified(')
        snippet = html[start:html.index('\n', end)]
        harness = r'''
import vm from 'node:vm';
import assert from 'node:assert/strict';
import { createProof } from 'ACCESS';
const context = vm.createContext({ TextEncoder, crypto: globalThis.crypto, Uint8Array, Error, Array, parseInt });
vm.runInContext(SNIPPET + ';globalThis.viewer={createProof,verified,setExpected:value=>{expected=value;},setPassword:value=>{password=value;},state:()=>({password,access})};', context);
const fp = side => Array.from({ length: 32 }, (_, i) => ((i * 7 + side) & 255).toString(16).padStart(2, '0').toUpperCase()).join(':');
const offer = `v=0\r\na=fingerprint:sha-256 ${fp(1)}\r\n`, answer = `v=0\r\na=fingerprint:sha-256 ${fp(2)}\r\n`;
const challenge = { type: 'auth_challenge', algorithm: 'camera-scram-sha256-v2', camera: '020000000001',
  salt: '00112233445566778899aabbccddeeff', nonce: 'ab'.repeat(32), iterations: 210000 };
const password = 'synthetic-test-vector';
const viewer = context.viewer;
await assert.rejects(viewer.createProof(challenge, offer, answer), /Enter the camera password/);
viewer.setPassword(password);
const local = await viewer.createProof(challenge, offer, answer);
const remote = await createProof(password, challenge.camera, challenge, offer, answer);
assert.equal(local.proof, remote.proof);
assert.equal(Buffer.from(local.expected).toString('hex'), remote.expected);
// The typed password is dropped; only a non-extractable derived key remains.
assert.equal(viewer.state().password, '');
assert.equal(viewer.state().access.key.extractable, false);
await assert.rejects(crypto.subtle.exportKey('raw', viewer.state().access.key));
// Reconnects reuse the key, and a new challenge gives a new proof.
assert.equal((await viewer.createProof(challenge, offer, answer)).proof, remote.proof);
const fresh = { ...challenge, nonce: 'cd'.repeat(32) };
assert.equal((await viewer.createProof(fresh, offer, answer)).proof, (await createProof(password, fresh.camera, fresh, offer, answer)).proof);
assert.notEqual((await viewer.createProof(fresh, offer, answer)).proof, remote.proof);
await assert.rejects(viewer.createProof({ ...challenge, camera: '020000000002' }, offer, answer), /Enter the camera password/);
viewer.setPassword('another-synthetic-vector');
assert.notEqual((await viewer.createProof(challenge, offer, answer)).proof, remote.proof);
await assert.rejects(viewer.createProof({ ...challenge, camera: 'not-a-camera' }, offer, answer));
await assert.rejects(viewer.createProof(challenge, offer + offer, answer));
assert.equal(context.viewer.verified(remote.expected), false, 'No signature is accepted before a challenge');
context.viewer.setExpected(new Uint8Array(0));
assert.equal(context.viewer.verified('00'.repeat(32)), false, 'An empty expectation must not match');
context.viewer.setExpected(local.expected);
assert.equal(context.viewer.verified(remote.expected), true);
assert.equal(context.viewer.verified('00'.repeat(32)), false);
'''
        script = harness.replace('ACCESS', (ROOT / 'site/src/projects/webrtc/access.mjs').as_uri()).replace('SNIPPET', json.dumps(snippet))
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)

    def test_every_viewer_must_authenticate(self):
        main = (ROOT / 'projects/webrtc/main/main.cpp').read_text()
        local_peer = main[main.index('esp_err_t session('):main.index('void remote_events()')]
        remote_peer = main[main.index('void remote_events()'):main.index('void start_server()')]
        for peer in (local_peer, remote_peer):
            self.assertRegex(peer, r'cfg\.on_channel_open\s*=\s*on_channel;')
            self.assertRegex(peer, r'cfg\.on_data\s*=\s*on_data;')
        channel_open = main[main.index('int on_channel('):main.index('int on_data(')]
        self.assertIn('send_text(peer_auth.begin(peer_offer, answer));', channel_open)
        self.assertNotIn('remote_peer', channel_open)

        # Both peers must verify the password and return before commands are queued.
        receive = main[main.index('int on_data('):main.index('void close_peer() {')]
        self.assertNotIn('remote_peer', receive)
        before_enqueue = receive[:receive.index('commands.emplace_back(')]
        self.assertRegex(
            before_enqueue,
            r'if\s*\(\s*!peer_auth\.allowed\(\)\s*\)\s*\{\s*'
            r'send_text\(peer_auth\.verify\(request\)\);\s*'
            r'cJSON_Delete\(request\);\s*return 0;\s*\}',
        )
        self.assertIn('const bool viewer = channel_ready && peer_auth.allowed();', main)
        self.assertNotIn('httpd_start(', main, 'The viewer is served over HTTPS only')
        viewer = (ROOT / 'projects/webrtc/main/viewer.html').read_text()
        self.assertIn('type="password"', viewer)
        self.assertNotIn('No pairing or access key', viewer)
