import { useCallback, useEffect, useRef, useState } from 'react';
import '../webrtc/webrtc.css';
import './flasher.css';
import { SiteHeader } from '../../components/SiteHeader';
import { Icon } from '../../components/Icon';
import { boards, firmwares } from 'virtual:board-catalog';
import { checkLayout, chipId, chipName, inspectImage, isEsp32Port, md5, parseFlasherArgs, portLabel } from './plan.mjs';

const decoder = new TextDecoder();
const bauds = [115200, 230400, 460800, 921600];
const kib = bytes => `${(bytes / 1024).toLocaleString(undefined, { maximumFractionDigits: 1 })} KiB`;

// Firmware packaged by tools/firmware_artifacts.py and served with the site.
function hostedRoot(project, board) {
  return { label: board.name, board: board.id, async read(path) {
    const response = await fetch(`firmware/${project}/${board.id}/${path}`, { cache: 'no-cache' });
    return response.ok ? new Uint8Array(await response.arrayBuffer()) : null;
  } };
}
const builtDate = seconds => new Date(seconds * 1000).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });

// One .bin chosen by the user; it stays in this browser and goes only to the board.
// An app image for a catalog project goes to that project's app partition.
async function loadUpload(file, packages) {
  const data = new Uint8Array(await file.arrayBuffer());
  const image = inspectImage(data, file.name);
  const known = image.kind === 'app' && firmwares.find(item => item.id === image.project);
  if (known) {
    const offset = Object.values(packages?.[known.id] ?? {}).map(entry => Number(entry.app_offset)).find(Number.isInteger);
    if (offset) image.address = offset;
  }
  return {
    chip: image.chip, flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep', before: 'default_reset', after: 'hard_reset',
    label: file.name, upload: image, firmware: known || null, files: [{ address: image.address, path: file.name, data }],
  };
}

// #/tools/flasher?project=<id> opens a catalog project directly.
function linkedProject() {
  const id = new URLSearchParams(window.location.hash.split('?')[1] ?? '').get('project');
  return id === 'upload' || firmwares.some(item => item.id === id) ? id : null;
}

async function loadFirmware(root) {
  const args = await root.read('flasher_args.json');
  if (!args) throw Error('This board has no firmware package yet');
  const plan = parseFlasherArgs(args);
  const data = await Promise.all(plan.files.map(file => root.read(file.path)));
  const missing = plan.files.find((file, index) => !data[index]);
  if (missing) throw Error(`${missing.path} is missing; rebuild this firmware first`);
  checkLayout(plan, data.map(bytes => bytes.length));
  let selection = null;
  try { const bytes = await root.read('firmware_selection.json'); if (bytes) selection = JSON.parse(decoder.decode(bytes)); } catch { selection = null; }
  if (root.board && selection?.board !== root.board) throw Error(`This package is not built for ${root.label}; rebuild its firmware`);
  return { ...plan, label: root.label, selection, files: plan.files.map((file, index) => ({ ...file, data: data[index] })) };
}

export function FlasherProject() {
  const supported = typeof navigator !== 'undefined' && 'serial' in navigator && window.isSecureContext;
  const [packages, setPackages] = useState(null);
  const [project, setProject] = useState(() => linkedProject() ?? firmwares.find(item => item.id === 'webrtc')?.id ?? firmwares[0]?.id ?? 'upload');
  const offered = firmwares.find(item => item.id === project);
  const offeredBoards = boards.filter(board => offered?.boards.includes(board.id));
  const [boardId, setBoardId] = useState('');
  const [firmware, setFirmware] = useState(null);
  const [firmwareError, setFirmwareError] = useState('');
  const [port, setPort] = useState(null);
  const [baud, setBaud] = useState(460800);
  const [eraseAll, setEraseAll] = useState(false);
  const [state, setState] = useState('idle');
  const [progress, setProgress] = useState(0);
  const [status, setStatus] = useState('');
  const [log, setLog] = useState([]);
  const fileInput = useRef(null);
  const busy = state === 'flashing';
  const load = useRef(0);

  useEffect(() => {
    let active = true;
    fetch('firmware/index.json', { cache: 'no-cache' })
      .then(response => response.ok ? response.json() : {})
      .catch(() => ({}))
      .then(manifest => { if (active) setPackages(manifest ?? {}); });
    return () => { active = false; };
  }, []);

  // One board at a time: keep the selected port while it stays connected, and
  // reuse a single port this site was allowed before.
  const refreshPort = useCallback(async () => {
    if (!supported) return;
    const allowed = await navigator.serial.getPorts();
    setPort(current => allowed.includes(current) ? current : allowed.length === 1 ? allowed[0] : null);
  }, [supported]);
  useEffect(() => {
    if (!supported) return undefined;
    refreshPort();
    navigator.serial.addEventListener('connect', refreshPort);
    navigator.serial.addEventListener('disconnect', refreshPort);
    return () => { navigator.serial.removeEventListener('connect', refreshPort); navigator.serial.removeEventListener('disconnect', refreshPort); };
  }, [supported, refreshPort]);
  useEffect(() => {
    if (!busy) return undefined;
    const warn = event => { event.preventDefault(); event.returnValue = ''; };
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [busy]);

  async function show(loading) {
    const ticket = ++load.current;
    setFirmware(null); setFirmwareError(''); setState('idle'); setStatus(''); setProgress(0);
    try {
      const loaded = await loading;
      if (ticket !== load.current) return;
      // Erasing everything would remove the bootloader that an app image needs.
      if (loaded.upload?.kind === 'app') setEraseAll(false);
      setFirmware(loaded);
    } catch (error) { if (ticket === load.current) setFirmwareError(error.message); }
  }
  function selectBoard(board) {
    setBoardId(board.id);
    show(loadFirmware(hostedRoot(project, board)));
  }
  function uploadFile(file) {
    if (!file) return;
    setBoardId('');
    show(loadUpload(file, packages));
  }
  async function selectPort() {
    try {
      const chosen = await navigator.serial.requestPort();
      // Forget the previous board's port so only the selected board stays allowed.
      if (port && port !== chosen) { try { await port.forget?.(); } catch { /* Already gone. */ } }
      setPort(chosen);
    } catch (error) { if (error.name !== 'NotFoundError') setStatus(error.message); }
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

  const portInfo = port?.getInfo() ?? {};
  const ready = supported && firmware && port && !busy;
  const selectedBoard = boards.find(board => board.id === boardId);
  const info = boardId && packages?.[project]?.[boardId];
  const upload = firmware?.upload;
  const next = upload ? firmware.firmware : offered;

  return <div className="rtc-theme sensorfan"><div className="rtc-body"><SiteHeader project={{ id: 'flasher', name: 'Flasher' }} /><main id="main-content" tabIndex="-1" className="rtc-content flasher">
    <div className="sensorfan-intro"><h1>Flash firmware</h1><p>Write firmware to an ESP32 board over USB, entirely in this browser. Choose a board, or upload your own firmware file. Firmware goes only from this page to the board; it is never sent to a server.</p></div>
    {!supported && <div className="rtc-panel flasher-warning" role="alert"><h2>This browser cannot flash boards</h2><p>Flashing uses Web Serial. Open this page in Chrome or Edge on a computer, over HTTPS or localhost.</p></div>}

    <section className="rtc-panel flasher-step" aria-labelledby="firmware-title">
      <div className="panel-title"><span className="flasher-number">1</span><h2 id="firmware-title">Firmware</h2></div>
      <div className="flasher-source"><label htmlFor="firmware-source">Firmware source</label><select id="firmware-source" disabled={busy} value={project} onChange={event => { setProject(event.target.value); window.history.replaceState(null, '', `#/tools/flasher?project=${event.target.value}`); setBoardId(''); load.current++; setFirmware(null); setFirmwareError(''); setState('idle'); setStatus(''); }}>{firmwares.map(item => <option key={item.id} value={item.id}>{item.name}</option>)}<option value="upload">Upload your firmware</option></select></div>
      {offered && <><p>{offered.description} Choose your board below.</p>
      <fieldset className="flasher-boards" disabled={busy}><legend className="flasher-hidden">Board</legend>{offeredBoards.map(board => {
        const available = packages?.[project]?.[board.id];
        return <label key={board.id} className="flasher-board" data-available={available ? 'true' : 'false'}>
          <input type="radio" name="board" checked={boardId === board.id} disabled={!available} onChange={() => selectBoard(board)} />
          {board.image ? <img src={board.image} alt="" /> : <span className="flasher-board-blank" aria-hidden="true"><Icon name="chip" size={28} /></span>}
          <span className="flasher-board-text"><strong>{board.name}</strong><small>{chipName(chipId(board.target))} · {packages === null ? 'Checking firmware…' : available ? 'Firmware ready' : 'No firmware yet'}</small></span>
        </label>;
      })}</fieldset>
      {packages && !Object.keys(packages[project] ?? {}).length && <p className="flasher-note">Firmware downloads are not available yet. You can select <strong>Upload your firmware</strong> above to use a file you already have.</p>}
      </>}
      {project === 'upload' && <div className="flasher-upload">
        <h3>Upload your firmware</h3>
        <p>Choose one <code>.bin</code> file from your computer. It stays in your browser and is sent only to the USB board.</p>
        <button type="button" className="button rtc-secondary" disabled={busy} onClick={() => fileInput.current?.click()}><Icon name="plus" size={16} />Upload firmware file</button>
        <input ref={fileInput} type="file" accept=".bin,application/octet-stream" hidden aria-label="Firmware file" onChange={event => { uploadFile(event.target.files[0]); event.target.value = ''; }} />
      </div>}
      {firmwareError && <p className="flasher-error" role="alert">{firmwareError}</p>}
      {firmware && <div className="flasher-firmware" data-testid="firmware-summary">
        <dl>{selectedBoard && <div><dt>Board</dt><dd>{selectedBoard.name}</dd></div>}{upload && <div><dt>File</dt><dd>{firmware.label}</dd></div>}<div><dt>Chip</dt><dd>{chipName(firmware.chip)}</dd></div>{upload && <div><dt>Type</dt><dd>{upload.kind === 'app' ? `App image${upload.project ? ` · ${upload.project}` : ''}${upload.version ? ` ${upload.version}` : ''}` : 'Full flash image'}</dd></div>}{info && <div><dt>Built</dt><dd>{builtDate(info.built)} · {info.commit}</dd></div>}<div><dt>Flash</dt><dd>{upload ? "Board's current settings" : `${firmware.flashSize} · ${firmware.flashMode.toUpperCase()} · ${firmware.flashFreq}`}</dd></div></dl>
        {upload?.kind === 'app' && <p className="flasher-note">The board must already have a bootloader and partition table with an app at 0x{upload.address.toString(16)}, for example from flashing one of the boards above once.</p>}
        {info && <a className="button rtc-secondary" href={`firmware/${project}/${boardId}/firmware.bin`} download={`${project}-${boardId}.bin`}>Download firmware file</a>}
        {upload?.kind === 'full' && <p className="flasher-note">A complete flash image replaces existing firmware and settings, including saved Wi-Fi credentials.</p>}
        <table><thead><tr><th>Offset</th><th>File</th><th>Size</th></tr></thead><tbody>{firmware.files.map(file => <tr key={file.address}><td>0x{file.address.toString(16)}</td><td>{file.path}</td><td>{kib(file.data.length)}</td></tr>)}</tbody></table>
      </div>}
    </section>

    <section className="rtc-panel flasher-step" aria-labelledby="port-title">
      <div className="panel-title"><span className="flasher-number">2</span><h2 id="port-title">USB port</h2></div>
      <p>Connect one board by USB, then select its port in the browser's list.</p>
      {port && <p className="flasher-selected-port" data-testid="selected-port"><Icon name="chip" size={16} /><span><strong>{portLabel(portInfo)}</strong>{isEsp32Port(portInfo) && <small>ESP32 programmer</small>}</span></p>}
      <button type="button" className="button rtc-secondary" disabled={!supported || busy} onClick={selectPort}><Icon name="plus" size={16} />Select USB port</button>
    </section>

    <section className="rtc-panel flasher-step" aria-labelledby="flash-title">
      <div className="panel-title"><span className="flasher-number">3</span><h2 id="flash-title">Flash</h2></div>
      <div className="flasher-options">
        <label htmlFor="flash-baud">Speed</label>
        <select id="flash-baud" value={baud} disabled={busy} onChange={event => setBaud(Number(event.target.value))}>{bauds.map(value => <option key={value} value={value}>{value.toLocaleString()} baud</option>)}</select>
        <label className="flasher-check"><input type="checkbox" checked={eraseAll} disabled={busy || upload?.kind === 'app'} onChange={event => setEraseAll(event.target.checked)} /><span>Erase entire flash first <small>{upload?.kind === 'app' ? 'Unavailable for app images, which need the bootloader already on the board' : 'Also clears saved Wi-Fi, camera password, and camera settings'}</small></span></label>
      </div>
      <button type="button" className="button rtc-primary flasher-go" disabled={!ready} onClick={flash}>{busy ? 'Flashing…' : 'Flash firmware'}</button>
      {state !== 'idle' && <div className="flasher-progress" data-state={state}><progress max="1" value={progress} aria-label="Flashing progress" /><span>{Math.round(progress * 100)}%</span></div>}
      <p role="status" className="flasher-status" data-testid="flash-status">{status}</p>
      {state === 'done' && next?.afterFlash.text && <p>{next.afterFlash.text} {next.afterFlash.link && <a href={next.afterFlash.link}>Open {next.name}</a>}</p>}
      {log.length > 0 && <details className="flasher-log"><summary>Details</summary><pre>{log.join('\n')}</pre></details>}
      <p className="flasher-hint">If the board doesn't connect, hold its BOOT button while selecting <strong>Flash firmware</strong>, or try a lower speed.</p>
    </section>
  </main></div></div>;
}
