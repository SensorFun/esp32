import { useCallback, useEffect, useRef, useState } from 'react';
import '../webrtc/webrtc.css';
import './flasher.css';
import { SiteHeader } from '../../components/SiteHeader';
import { Icon } from '../../components/Icon';
import { checkLayout, chipId, chipName, firmwareRoots, isEsp32Port, md5, parseFlasherArgs, portLabel } from './plan.mjs';

const decoder = new TextDecoder();
const skipped = new Set(['node_modules', '.git', '.venv', 'managed_components', 'CMakeFiles', 'esp-idf', 'config', 'bootloader', 'partition_table']);
const bauds = [115200, 230400, 460800, 921600];
const kib = bytes => `${(bytes / 1024).toLocaleString(undefined, { maximumFractionDigits: 1 })} KiB`;

// A firmware root is a folder holding flasher_args.json, readable by relative path.
async function rootsFromDirectory(handle) {
  const roots = [];
  async function visit(dir, prefix, depth) {
    let hasArgs = false;
    const children = [];
    for await (const [name, entry] of dir.entries()) {
      if (entry.kind === 'file' && name === 'flasher_args.json') hasArgs = true;
      else if (entry.kind === 'directory' && depth < 3 && !name.startsWith('.') && !skipped.has(name)) children.push([name, entry]);
    }
    if (hasArgs) {
      roots.push({ label: prefix || handle.name, async read(path) {
        try {
          const parts = path.split('/');
          let folder = dir;
          for (const part of parts.slice(0, -1)) folder = await folder.getDirectoryHandle(part);
          const file = await (await folder.getFileHandle(parts.at(-1))).getFile();
          return new Uint8Array(await file.arrayBuffer());
        } catch { return null; }
      } });
      return;
    }
    for (const [name, entry] of children) await visit(entry, prefix ? `${prefix}/${name}` : name, depth + 1);
  }
  await visit(handle, '', 0);
  return roots.sort((a, b) => a.label.localeCompare(b.label));
}

function rootsFromFiles(fileList) {
  const files = new Map();
  for (const file of fileList) files.set(file.webkitRelativePath.split('/').slice(1).join('/') || file.name, file);
  const top = fileList[0]?.webkitRelativePath.split('/')[0] || 'Selected folder';
  return firmwareRoots([...files.keys()]).map(root => ({ label: root || top, async read(path) {
    const file = files.get(root ? `${root}/${path}` : path);
    return file ? new Uint8Array(await file.arrayBuffer()) : null;
  } }));
}

async function loadFirmware(root) {
  const args = await root.read('flasher_args.json');
  if (!args) throw Error('flasher_args.json is missing from this folder');
  const plan = parseFlasherArgs(args);
  const data = await Promise.all(plan.files.map(file => root.read(file.path)));
  const missing = plan.files.find((file, index) => !data[index]);
  if (missing) throw Error(`${missing.path} is missing; rebuild this firmware first`);
  checkLayout(plan, data.map(bytes => bytes.length));
  let selection = null;
  try { const bytes = await root.read('firmware_selection.json'); if (bytes) selection = JSON.parse(decoder.decode(bytes)); } catch { selection = null; }
  return { ...plan, label: root.label, selection, files: plan.files.map((file, index) => ({ ...file, data: data[index] })) };
}

export function FlasherProject() {
  const supported = typeof navigator !== 'undefined' && 'serial' in navigator && window.isSecureContext;
  const [roots, setRoots] = useState([]);
  const [rootIndex, setRootIndex] = useState(-1);
  const [firmware, setFirmware] = useState(null);
  const [firmwareError, setFirmwareError] = useState('');
  const [ports, setPorts] = useState([]);
  const [port, setPort] = useState(null);
  const [baud, setBaud] = useState(460800);
  const [eraseAll, setEraseAll] = useState(false);
  const [state, setState] = useState('idle');
  const [progress, setProgress] = useState(0);
  const [status, setStatus] = useState('');
  const [log, setLog] = useState([]);
  const folderInput = useRef(null);
  const busy = state === 'flashing';

  const refreshPorts = useCallback(async () => {
    if (!supported) return;
    const list = await navigator.serial.getPorts();
    setPorts(list);
    setPort(current => list.includes(current) ? current : list.length === 1 ? list[0] : null);
  }, [supported]);
  useEffect(() => {
    if (!supported) return undefined;
    refreshPorts();
    navigator.serial.addEventListener('connect', refreshPorts);
    navigator.serial.addEventListener('disconnect', refreshPorts);
    return () => { navigator.serial.removeEventListener('connect', refreshPorts); navigator.serial.removeEventListener('disconnect', refreshPorts); };
  }, [supported, refreshPorts]);
  useEffect(() => {
    if (!busy) return undefined;
    const warn = event => { event.preventDefault(); event.returnValue = ''; };
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [busy]);

  async function applyRoots(found) {
    setRoots(found); setFirmware(null); setFirmwareError(''); setRootIndex(-1);
    if (!found.length) { setFirmwareError('No firmware found. Choose build/webrtc or a board folder inside it that contains flasher_args.json.'); return; }
    if (found.length === 1) await selectRoot(found, 0);
  }
  async function selectRoot(list, index) {
    setRootIndex(index); setFirmware(null); setFirmwareError('');
    try { setFirmware(await loadFirmware(list[index])); } catch (error) { setFirmwareError(error.message); }
  }
  async function chooseFolder() {
    if (!window.showDirectoryPicker) { folderInput.current?.click(); return; }
    try { await applyRoots(await rootsFromDirectory(await window.showDirectoryPicker({ id: 'esp32-firmware', mode: 'read' }))); }
    catch (error) { if (error.name !== 'AbortError') setFirmwareError(error.message); }
  }
  async function addPort() {
    try {
      const chosen = await navigator.serial.requestPort();
      await refreshPorts();
      setPort(chosen);
    } catch (error) { if (error.name !== 'NotFoundError') setStatus(error.message); }
  }
  async function forgetPort(target) {
    await target.forget?.();
    await refreshPorts();
  }

  async function flash() {
    if (!firmware || !port || busy) return;
    setState('flashing'); setProgress(0); setLog([]); setStatus('Connecting to the board…');
    const lines = [];
    let partial = '';
    const push = text => { lines.push(text); if (lines.length > 500) lines.shift(); setLog([...lines]); };
    const terminal = { clean() { lines.length = 0; setLog([]); }, writeLine(text) { push(partial + text); partial = ''; }, write(text) { partial += text; } };
    const total = firmware.files.reduce((sum, file) => sum + file.data.length, 0);
    const before = firmware.files.map((_, index) => firmware.files.slice(0, index).reduce((sum, file) => sum + file.data.length, 0));
    let transport;
    try {
      const { ESPLoader, Transport } = await import('esptool-js');
      transport = new Transport(port, false);
      const loader = new ESPLoader({ transport, baudrate: baud, romBaudrate: 115200, terminal });
      const description = await loader.main(firmware.before);
      if (chipId(loader.chip.CHIP_NAME) !== firmware.chip) {
        throw Error(`This firmware is for ${chipName(firmware.chip)}, but the board is ${description}. Nothing was written.`);
      }
      setStatus(eraseAll ? 'Erasing flash and writing firmware…' : 'Writing firmware…');
      await loader.writeFlash({
        fileArray: firmware.files.map(file => ({ data: file.data, address: file.address })),
        flashMode: firmware.flashMode, flashFreq: firmware.flashFreq, flashSize: firmware.flashSize,
        eraseAll, compress: true, calculateMD5Hash: md5,
        reportProgress: (index, written, size) => setProgress((before[index] + (size ? written / size : 1) * firmware.files[index].data.length) / total),
      });
      setStatus('Restarting the board…');
      await loader.after(firmware.after);
      setProgress(1); setState('done');
      setStatus('Done. Firmware written and verified; the board has restarted.');
    } catch (error) {
      setState('error');
      const message = error?.message || String(error);
      setStatus(/Failed to open serial port|already open|Access denied|NetworkError/i.test(message)
        ? `${message} Close other programs using this port (serial monitors, idf.py monitor, Arduino IDE) and try again.`
        : message);
    } finally {
      if (partial) push(partial);
      try { await transport?.disconnect(); } catch { /* The port may already be closed. */ }
    }
  }

  const labels = ports.map(item => portLabel(item.getInfo()));
  const named = ports.map((item, index) => labels.filter(label => label === labels[index]).length > 1 ? `${labels[index]} #${labels.slice(0, index + 1).filter(label => label === labels[index]).length}` : labels[index]);
  const ready = supported && firmware && port && !busy;

  return <div className="rtc-theme sensorfan"><div className="rtc-body"><SiteHeader project={{ id: 'flasher', name: 'Flasher' }} /><main id="main-content" tabIndex="-1" className="rtc-content flasher">
    <div className="sensorfan-intro"><h1>Flash firmware</h1><p>Write firmware you built to an ESP32 board over USB. Files are read from this computer and sent only to the board; nothing is uploaded.</p></div>
    {!supported && <div className="rtc-panel flasher-warning" role="alert"><h2>This browser cannot flash boards</h2><p>Flashing uses Web Serial. Open this page in Chrome or Edge on a computer, over HTTPS or localhost.</p></div>}

    <section className="rtc-panel flasher-step" aria-labelledby="firmware-title">
      <div className="panel-title"><span className="flasher-number">1</span><h2 id="firmware-title">Firmware</h2></div>
      <p>Choose your local <code>build/webrtc</code> folder, or one board folder inside it. Build first with <code>python3 tools/build.py project=webrtc device=…</code>.</p>
      <button type="button" className="button rtc-secondary" disabled={busy} onClick={chooseFolder}><Icon name="chip" size={16} />Choose build folder</button>
      <input ref={folderInput} type="file" webkitdirectory="" directory="" multiple hidden onChange={event => { applyRoots(rootsFromFiles([...event.target.files])); event.target.value = ''; }} />
      {roots.length > 1 && <fieldset className="flasher-choices" disabled={busy}><legend>Builds in this folder</legend>{roots.map((root, index) => <label key={root.label}><input type="radio" name="firmware-root" checked={rootIndex === index} onChange={() => selectRoot(roots, index)} /><span>{root.label}</span></label>)}</fieldset>}
      {firmwareError && <p className="flasher-error" role="alert">{firmwareError}</p>}
      {firmware && <div className="flasher-firmware" data-testid="firmware-summary">
        <dl><div><dt>Build</dt><dd>{firmware.label}</dd></div><div><dt>Chip</dt><dd>{chipName(firmware.chip)}</dd></div>{firmware.selection?.board && <div><dt>Board</dt><dd>{firmware.selection.board}</dd></div>}{firmware.selection?.frame_grabber && <div><dt>Frame grabber</dt><dd>{firmware.selection.frame_grabber.toUpperCase()}</dd></div>}<div><dt>Flash</dt><dd>{firmware.flashSize} · {firmware.flashMode.toUpperCase()} · {firmware.flashFreq}</dd></div></dl>
        <table><thead><tr><th>Offset</th><th>File</th><th>Size</th></tr></thead><tbody>{firmware.files.map(file => <tr key={file.address}><td>0x{file.address.toString(16)}</td><td>{file.path}</td><td>{kib(file.data.length)}</td></tr>)}</tbody></table>
      </div>}
    </section>

    <section className="rtc-panel flasher-step" aria-labelledby="port-title">
      <div className="panel-title"><span className="flasher-number">2</span><h2 id="port-title">USB port</h2></div>
      <p>Connect the board by USB, then choose its port. The browser lists every serial port on this computer; ports you allow stay listed here.</p>
      {ports.length > 0 && <fieldset className="flasher-choices" disabled={busy}><legend>Allowed ports</legend>{ports.map((item, index) => <div className="flasher-port" key={index}><label><input type="radio" name="serial-port" checked={port === item} onChange={() => setPort(item)} /><span>{named[index]}{isEsp32Port(item.getInfo()) && <small>ESP32 programmer</small>}</span></label>{item.forget && <button type="button" className="flasher-forget" onClick={() => forgetPort(item)} aria-label={`Forget ${named[index]}`}>Forget</button>}</div>)}</fieldset>}
      <button type="button" className="button rtc-secondary" disabled={!supported || busy} onClick={addPort}><Icon name="plus" size={16} />Add USB port</button>
    </section>

    <section className="rtc-panel flasher-step" aria-labelledby="flash-title">
      <div className="panel-title"><span className="flasher-number">3</span><h2 id="flash-title">Flash</h2></div>
      <div className="flasher-options">
        <label htmlFor="flash-baud">Speed</label>
        <select id="flash-baud" value={baud} disabled={busy} onChange={event => setBaud(Number(event.target.value))}>{bauds.map(value => <option key={value} value={value}>{value.toLocaleString()} baud</option>)}</select>
        <label className="flasher-check"><input type="checkbox" checked={eraseAll} disabled={busy} onChange={event => setEraseAll(event.target.checked)} /><span>Erase entire flash first <small>Also clears saved Wi-Fi, camera password, and camera settings</small></span></label>
      </div>
      <button type="button" className="button rtc-primary flasher-go" disabled={!ready} onClick={flash}>{busy ? 'Flashing…' : 'Flash firmware'}</button>
      {state !== 'idle' && <div className="flasher-progress" data-state={state}><progress max="1" value={progress} aria-label="Flashing progress" /><span>{Math.round(progress * 100)}%</span></div>}
      <p role="status" className="flasher-status" data-testid="flash-status">{status}</p>
      {state === 'done' && <p>Next, set up Wi-Fi from the board's setup network, then open it in <a href="#/projects/webrtc">WebRTC</a>.</p>}
      {log.length > 0 && <details className="flasher-log"><summary>Details</summary><pre>{log.join('\n')}</pre></details>}
      <p className="flasher-hint">If the board doesn't connect, hold its BOOT button while selecting <strong>Flash firmware</strong>, or try a lower speed.</p>
    </section>
  </main></div></div>;
}
