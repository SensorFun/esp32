// Test launcher: source mounts are read-only and no ports are published.
import { randomBytes } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { mkdtempSync, writeFileSync, rmSync, rmdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { setTimeout as delay } from 'node:timers/promises';
const folder = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(folder, '../../..');
const environment = { ...process.env, SIGNALING_API_SECRET: randomBytes(32).toString('hex'),
  TEST_VIEWER_URL: 'ws://127.0.0.1:8080/viewer', TEST_CAMERA_URL: 'ws://127.0.0.1:8080/camera' };
function docker(args) {
  const result = spawnSync('docker', args, { encoding: 'utf8', env: environment });
  if (result.status !== 0) throw Error(result.stderr);
  return result.stdout.trim();
}
const mount = (source, target) => ['--mount', `type=bind,source=${source},target=${target},readonly`];
let server;
const secretsFolder = mkdtempSync(path.join(tmpdir(), 'signaling-test-'));
const secretFile = path.join(secretsFolder, 'signaling_api_secret');
writeFileSync(secretFile, environment.SIGNALING_API_SECRET, { mode: 0o600 });
try {
  server = docker(['run', '-d', '--user', `${process.getuid()}:${process.getgid()}`, '--read-only', '--cap-drop=ALL',
    ...mount(secretFile, '/run/secrets/signaling_api_secret'),
    ...mount(`${root}/tools/signal_server/server.py`, '/opt/signaling/server.py'), 'esp32-signaling:local']);
  for (let attempt = 0; attempt < 50; attempt++) {
    const ready = spawnSync('docker', ['exec', server, 'python3', '-c', 'import socket; socket.create_connection(("127.0.0.1",8080),1).close()']);
    if (ready.status === 0) break;
    if (attempt === 49) throw Error('Test service did not start');
    await delay(100);
  }
  const result = spawnSync('docker', ['run', '--rm', '--read-only', '--cap-drop=ALL', '--network', `container:${server}`,
    '-e', 'SIGNALING_API_SECRET', '-e', 'TEST_VIEWER_URL', '-e', 'TEST_CAMERA_URL',
    ...mount(`${folder}/client-test.mjs`, '/workspace/tests/webrtc/signaling/client-test.mjs'),
    ...mount(`${root}/site/src/projects/webrtc/signaling.mjs`, '/workspace/site/src/projects/webrtc/signaling.mjs'),
    ...mount(`${root}/site/node_modules/ws`, '/workspace/site/node_modules/ws'),
    'node:22-bookworm-slim', 'node', '--test', '/workspace/tests/webrtc/signaling/client-test.mjs'], { env: environment, stdio: 'inherit' });
  process.exitCode = result.status ?? 1;
  // The activity log records connections, rejections and a final summary,
  // but never the secret or a full room ID (rooms are join capabilities).
  docker(['stop', server]);
  const logs = spawnSync('docker', ['logs', server], { encoding: 'utf8' });
  const text = logs.stdout + logs.stderr;
  const problems = [];
  for (const expected of [/ started /, /camera_rejected reason=bad_secret/, /camera_connected room=[0-9a-f]{8} cameras_online=\d+/,
    /viewer_connected room=[0-9a-f]{8}/, /viewer_rejected reason=camera_not_found/, /viewer_disconnected room=[0-9a-f]{8} reason=\w+/,
    / stopping uptime=\S+ cameras_online=0 viewers_online=0 rooms=0 .*viewer_attempts=\d+/]) {
    if (!expected.test(text)) problems.push(`log lacks ${expected}`);
  }
  if (text.includes(environment.SIGNALING_API_SECRET)) problems.push('log contains the API secret');
  if (/[0-9A-F]{32}/.test(text)) problems.push('log contains a room ID');
  if (problems.length) { console.error(problems.join('\n') + '\n--- log ---\n' + text); process.exitCode = 1; }
  else console.log(`activity log ok (${text.trim().split('\n').length} lines)`);
} finally {
  if (server) spawnSync('docker', ['rm', '-f', server]);
  rmSync(secretFile);
  rmdirSync(secretsFolder);
}
