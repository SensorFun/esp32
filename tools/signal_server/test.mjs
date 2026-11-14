// Test launcher: source mounts are read-only and no ports are published.
import { randomBytes } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
const folder = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(folder, '../..');
const environment = { ...process.env, JANUS_API_SECRET: randomBytes(32).toString('hex'),
  JANUS_UPSTREAM_URL: 'ws://127.0.0.1:8188', TEST_JANUS_URL: 'ws://127.0.0.1:8188',
  TEST_VIEWER_URL: 'ws://127.0.0.1:8080/viewer', TEST_CAMERA_URL: 'ws://127.0.0.1:8080/camera' };
function docker(args) {
  const result = spawnSync('docker', args, { encoding: 'utf8', env: environment });
  if (result.status !== 0) throw Error(result.stderr);
  return result.stdout.trim();
}
const mount = (source, target) => ['--mount', `type=bind,source=${source},target=${target},readonly`];
let janus, gateway;
try {
  janus = docker(['run', '--rm', '-d', '--read-only', '--tmpfs', '/tmp:mode=1777', '--cap-drop=ALL', '-e', 'JANUS_API_SECRET',
    ...mount(`${folder}/entrypoint.py`, '/opt/signaling/entrypoint.py'), 'esp32-signaling:local']);
  gateway = docker(['run', '--rm', '-d', '--read-only', '--cap-drop=ALL', '--network', `container:${janus}`,
    '-e', 'JANUS_API_SECRET', '-e', 'JANUS_UPSTREAM_URL', ...mount(`${folder}/gateway.py`, '/opt/signaling/gateway.py'),
    '--entrypoint', 'python3', 'esp32-signaling:local', '/opt/signaling/gateway.py']);
  for (let attempt = 0; attempt < 50; attempt++) {
    const ready = spawnSync('docker', ['exec', gateway, 'python3', '-c', 'import socket; socket.create_connection(("127.0.0.1",8080),1).close(); socket.create_connection(("127.0.0.1",8188),1).close()']);
    if (ready.status === 0) break;
    if (attempt === 49) throw Error('Test services did not start');
    await delay(100);
  }
  const result = spawnSync('docker', ['run', '--rm', '--read-only', '--cap-drop=ALL', '--network', `container:${janus}`,
    '-e', 'JANUS_API_SECRET', '-e', 'TEST_JANUS_URL', '-e', 'TEST_VIEWER_URL', '-e', 'TEST_CAMERA_URL',
    ...mount(`${folder}/client-test.mjs`, '/workspace/tools/signal_server/client-test.mjs'),
    ...mount(`${root}/site/src/spaces/webrtc/janus-signaling.mjs`, '/workspace/site/src/spaces/webrtc/janus-signaling.mjs'),
    ...mount(`${root}/site/node_modules/ws`, '/workspace/site/node_modules/ws'),
    'node:22-bookworm-slim', 'node', '--test', '/workspace/tools/signal_server/client-test.mjs'], { env: environment, stdio: 'inherit' });
  process.exitCode = result.status ?? 1;
} finally {
  if (gateway) docker(['stop', gateway]);
  if (janus) docker(['stop', janus]);
}
