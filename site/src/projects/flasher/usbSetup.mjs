// USB setup: the firmware's Wi-Fi setup page runs unchanged in the browser and
// its /api/* requests are answered over the board's serial console instead of
// the setup access point. Lines from the board look like `SENSORFUN_USB {json}`.
const prefix = 'SENSORFUN_USB ';
const encoder = new TextEncoder();
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const ipv4 = value => typeof value === 'string' && /^(\d{1,3}\.){3}\d{1,3}$/.test(value) && value.split('.').every(part => Number(part) < 256);

// Opens the port (after esptool has released it) and waits until the firmware answers.
export async function openUsbSession(port, signal) {
  await port.open({ baudRate: 115200 });
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();
  const decoder = new TextDecoder();
  let buffer = '', notify, readError, queue = Promise.resolve(), closed;
  const replies = [];
  const read = (async () => {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) throw Error('The USB connection closed.');
        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop().slice(-4096);
        for (const line of lines) {
          const start = line.indexOf(prefix);
          if (start < 0) continue;
          try { replies.push(JSON.parse(line.slice(start + prefix.length))); } catch { continue; }
          notify?.();
        }
      }
    } catch (error) { readError = error; notify?.(); }
  })();
  const stop = () => { notify?.(); void reader.cancel().catch(() => {}); };
  signal.addEventListener('abort', stop, { once: true });

  async function exchange(command, accept, timeout) {
    signal.throwIfAborted();
    replies.length = 0;
    await writer.write(encoder.encode(`sensorfan-usb ${command}\n`));
    const deadline = performance.now() + timeout;
    while (performance.now() < deadline) {
      signal.throwIfAborted();
      const reply = replies.shift();
      if (reply) { if (accept(reply)) return reply; continue; }
      if (readError) throw readError;
      await new Promise(resolve => {
        const timer = setTimeout(() => { notify = null; resolve(); }, Math.max(0, deadline - performance.now()));
        notify = () => { clearTimeout(timer); notify = null; resolve(); };
      });
    }
    return null;
  }
  // One command at a time; replies carry no request id.
  const send = (command, accept, timeout) => {
    const next = queue.then(() => exchange(command, accept, timeout));
    queue = next.catch(() => {});
    return next;
  };
  const status = () => send('status', reply => reply.status === true, 900);

  const session = {
    firmware: null,  // The first status reply: which project the board runs.
    status,
    send,
    async close() {
      if (closed) return closed;
      closed = (async () => {
        signal.removeEventListener('abort', stop);
        await reader.cancel().catch(() => {});
        await read;
        reader.releaseLock();
        writer.releaseLock();
        await port.close().catch(() => {});
      })();
      return closed;
    },
  };
  try {
    // Opening the port can restart the board; give it time to boot.
    let current;
    for (let attempt = 0; attempt < 6 && !current; attempt++) current = await status();
    if (!current && port.setSignals) {
      // Still in the bootloader (or hung): restart it into the firmware, with
      // IO0 high (DTR off) while EN is pulsed low (RTS), as esptool does.
      await port.setSignals({ dataTerminalReady: false, requestToSend: true });
      await pause(100);
      await port.setSignals({ requestToSend: false });
      for (let attempt = 0; attempt < 20 && !current; attempt++) current = await status();
    }
    if (!current) throw Error('The board did not answer over USB. Start it normally (release BOOT and reset), then try again.');
    session.firmware = current;
    return session;
  } catch (error) {
    await session.close();
    throw error;
  }
}

const json = (status, body) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });

// Answers the setup page's requests (see projects/common/provisioning/index.html)
// with the same JSON the setup access point returns. onConnected receives the
// local viewer address and the remote viewer address (if any); onFinish runs when the page's "open camera" link is used.
export function usbSetupApi(session, { onConnected, onFinish }) {
  return async function request(path, options = {}) {
    const method = (options.method || 'GET').toUpperCase();
    const signal = options.signal;
    signal?.throwIfAborted();
    if (method === 'GET' && path === '/api/scan') {
      // The board answers busy while it is still joining its saved network.
      const deadline = performance.now() + 30000;
      for (;;) {
        const reply = await session.send('scan', reply => ['scan', 'busy', 'scan_failed'].includes(reply.result), 12000);
        if (reply?.result === 'scan') {
          return json(200, { ssids: Array.isArray(reply.ssids) ? reply.ssids.filter(name => typeof name === 'string') : [],
            camera_password_required: reply.camera_password_required === true,
            camera_password_saved: reply.camera_password_saved === true });
        }
        if (reply?.result === 'scan_failed') return json(503, { error: 'Scan failed' });
        if (!reply) throw TypeError('The camera did not answer over USB.');
        if (performance.now() > deadline) return json(409, { error: 'Wait for the connection attempt to finish' });
        await pause(1000);
      }
    }
    if (method === 'POST' && path === '/api/connect') {
      const body = new URLSearchParams(options.body);
      const reply = await session.send(`wifi ${body}`, reply => !reply.status && reply.result !== 'scan', 4500);
      if (!reply) throw TypeError('The camera did not answer over USB.');
      if (reply.result === 'pending') return json(202, { state: 'connecting' });
      if (reply.result === 'busy') return json(409, { error: 'Connection already in progress' });
      return json(400, { error: 'Check Wi-Fi credentials, camera password (12–96 bytes)' });
    }
    if (method === 'GET' && path === '/api/status') {
      const reply = await session.status();
      if (!reply) throw TypeError('The camera did not answer over USB.');
      if (reply.result === 'pending' || (reply.result === 'saved' && !ipv4(reply.ip))) return json(200, { state: 'connecting' });
      if (reply.result === 'saved') {
        const url = `https://${reply.ip}/`;
        const remote = typeof reply.remote_url === 'string' && /^https:\/\/[^\s/]+\/\S*[0-9A-F]{12}$/.test(reply.remote_url) ? reply.remote_url : '';
        onConnected?.(url, remote);
        return json(200, { state: 'connected', ip: reply.ip, local_url: url, ...(remote ? { remote_url: remote } : {}) });
      }
      if (reply.result === 'restored') return json(409, { error: 'Could not join the new Wi-Fi network. The camera reconnected to its saved network.' });
      if (reply.result === 'storage_error') return json(500, { error: 'Could not save Wi-Fi settings; try again' });
      return json(200, { state: 'provisioning' });
    }
    if (method === 'POST' && path === '/api/finish') {
      onFinish?.();
      return json(200, {});
    }
    return json(404, { error: 'Not found' });
  };
}
