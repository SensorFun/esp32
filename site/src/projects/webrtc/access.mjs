const encoder = new TextEncoder();
export function normalizeMac(value) {
  const text = value.trim();
  if (!/^(?:[a-f\d]{12}|(?:[a-f\d]{2}:){5}[a-f\d]{2}|(?:[a-f\d]{2}-){5}[a-f\d]{2})$/i.test(text)) throw Error('Enter the camera Wi-Fi MAC address (six hexadecimal pairs).');
  return text.replace(/[:-]/g, '').toLowerCase();
}
export const roomForMac = value => normalizeMac(value).toUpperCase();
export const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
export function unhex(value, length) {
  if (typeof value !== 'string' || value.length !== length * 2 || !/^[a-f\d]+$/i.test(value)) throw Error('Invalid authentication response');
  return Uint8Array.from(value.match(/../g), pair => parseInt(pair, 16));
}
export function fingerprint(sdp) {
  if (typeof sdp !== 'string' || (sdp.match(/a=fingerprint:/gi) || []).length !== 1) throw Error('Ambiguous WebRTC certificate fingerprints');
  const value = /^a=fingerprint:sha-256 ([a-f\d:]+)\r?$/im.exec(sdp)?.[1].replace(/:/g, '').toLowerCase();
  if (!value || value.length !== 64) throw Error('Missing WebRTC certificate fingerprint');
  return value;
}
export function transcript(room, challenge, offer, answer) {
  if (!/^[A-F\d]{12}$/.test(room) || challenge.algorithm !== 'camera-scram-sha256-v2' || challenge.iterations !== 210000) throw Error('Unsupported camera authentication');
  unhex(challenge.salt, 16); unhex(challenge.nonce, 32);
  return encoder.encode(['camera-scram-v2', room, challenge.salt, challenge.iterations, challenge.nonce, fingerprint(offer), fingerprint(answer)].join('\n'));
}
async function hmac(key, data) {
  const imported = await crypto.subtle.importKey('raw', key, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return new Uint8Array(await crypto.subtle.sign('HMAC', imported, typeof data === 'string' ? encoder.encode(data) : data));
}
export async function deriveCameraAccess(camera, password) {
  if (!globalThis.crypto?.subtle) throw Error('Use HTTPS or localhost for camera access');
  const id = roomForMac(camera);
  const bytes = encoder.encode(password);
  if (bytes.length < 12 || bytes.length > 96) throw Error('Camera password must be 12–96 UTF-8 bytes');
  const salt = await crypto.subtle.digest('SHA-256', encoder.encode(`camera-kdf-v2\n${id}`));
  const input = await crypto.subtle.importKey('raw', bytes, 'PBKDF2', false, ['deriveBits']);
  bytes.fill(0);
  const root = new Uint8Array(await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations: 210000 }, input, 256));
  const room = hex((await hmac(root, `camera-room-v1\n${id}`)).slice(0, 16)).toUpperCase();
  const key = await crypto.subtle.importKey('raw', root, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  root.fill(0);
  return { id, room, key };
}
export async function createProof(password, camera, challenge, offer, answer, prepared) {
  const access = prepared || await deriveCameraAccess(camera, password);
  const text = transcript(access.id, challenge, offer, answer);
  const salted = new Uint8Array(await crypto.subtle.sign('HMAC', access.key, encoder.encode(`camera-auth-v2\n${challenge.salt}`)));
  const client = await hmac(salted, 'Client Key');
  const stored = await crypto.subtle.digest('SHA-256', client);
  const signature = await hmac(stored, text);
  const proof = client.map((byte, index) => byte ^ signature[index]);
  const server = await hmac(salted, 'Server Key');
  const expected = hex(await hmac(server, text));
  salted.fill(0); client.fill(0); server.fill(0);
  return { proof: hex(proof), expected };
}
export function verifyServer(signature, expected) {
  if (!expected) return false;
  try { const actual = unhex(signature, 32), wanted = unhex(expected, 32); let diff = 0; actual.forEach((byte, index) => { diff |= byte ^ wanted[index]; }); return diff === 0; } catch { return false; }
}
