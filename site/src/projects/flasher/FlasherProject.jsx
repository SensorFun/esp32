import { useCallback, useEffect, useRef, useState } from 'react';
import '../webrtc/webrtc.css';
import './flasher.css';
import { SiteHeader } from '../../components/SiteHeader';
import { Icon } from '../../components/Icon';
import { boards, firmwares } from 'virtual:board-catalog';
import { checkLayout, chipId, chipName, inspectImage, isEsp32Port, md5, parseFlasherArgs, portLabel } from './plan.mjs';
import { resetNvs } from './nvs.mjs';
import { UsbProvisioning } from './UsbProvisioning';

const decoder = new TextDecoder();
const bauds = [115200, 230400, 460800, 921600];

// Firmware packaged by tools/firmware_artifacts.py and served with the site.
function hostedRoot(project, board) {
  return { label: board.name, board: board.id, async read(path) {
    const response = await fetch(`firmware/${project}/${board.id}/${path}`, { cache: 'no-cache' });
    return response.ok ? new Uint8Array(await response.arrayBuffer()) : null;
  } };
}
// esptool-js 0.7's HardReset only releases RTS and never asserts it, so the
// board stayed in the flasher stub after "Board restarted". This is Python
// esptool's sequence: IO0 high (DTR off), then pulse EN low with RTS.
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const hardReset = (transport, usingUsbOtg) => ({
  async reset() {
    await transport.setDTR(false);
    await transport.setRTS(true);
    await pause(usingUsbOtg ? 200 : 100);
    await transport.setRTS(false);
    if (usingUsbOtg) await pause(200);
  },
});
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
  const [baud, setBaud] = useState(115200);
  const [connectionMode, setConnectionMode] = useState('automatic');
  const [clearSettings, setClearSettings] = useState(false);
  const [writeFirmware, setWriteFirmware] = useState(true);
  const [tableAddress, setTableAddress] = useState('0x8000');
  const [partitionName, setPartitionName] = useState('');
  const [state, setState] = useState('idle');
  const [progress, setProgress] = useState(0);
  const [status, setStatus] = useState('');
  const [log, setLog] = useState([]);
  const [showUsb, setShowUsb] = useState(false);
  const [viewerUrl, setViewerUrl] = useState('');
  const fileInput = useRef(null);
  const resetMode = state.startsWith('reset');
  const flashBusy = state === 'flashing' || state === 'resetting';
  const busy = flashBusy || showUsb;
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
      if (port !== chosen) { setState('idle'); setStatus(''); setLog([]); setProgress(0); setShowUsb(false); setViewerUrl(''); }
    } catch (error) { if (error.name !== 'NotFoundError') setStatus(error.message); }
  }

  async function applyFlashActions() {
    if (!ready) return;
    setState(writeFirmware ? 'flashing' : 'resetting');
    setProgress(0); setLog([]); setStatus('Connecting to the board…');
    const lines = [];
    let partial = '';
    const push = text => { lines.push(text); if (lines.length > 500) lines.shift(); setLog([...lines]); };
    const terminal = { clean() { lines.length = 0; setLog([]); }, writeLine(text) { push(partial + text); partial = ''; }, write(text) { partial += text; } };
    let transport, loader, connected = false, cleared = false;
    let plan = firmware;
    let resultState = writeFirmware ? 'error' : 'reset-error';
    let message;
    try {
      if (writeFirmware && info?.local) {
        setStatus('Loading the latest local firmware…');
        const [latest, manifest] = await Promise.all([
          loadFirmware(hostedRoot(project, selectedBoard)),
          fetch('firmware/index.json', { cache: 'no-cache' }).then(response => response.json()),
        ]);
        plan = latest;
        setFirmware(latest);
        setPackages(manifest);
        setStatus('Connecting to the board…');
      }
      const { ESPLoader, Transport } = await import('esptool-js');
      transport = new Transport(port, false);
      const manual = connectionMode === 'manual';
      loader = new ESPLoader({ transport, baudrate: manual ? 115200 : baud, romBaudrate: 115200, terminal, resetConstructors: { hardReset } });
      const description = await loader.main(manual ? 'no_reset' : plan?.before ?? 'default_reset');
      connected = true;
      if (writeFirmware && chipId(loader.chip.CHIP_NAME) !== plan.chip) {
        throw Error(`This firmware is for ${chipName(plan.chip)}, but the board is ${description}. Nothing changed.`);
      }
      if (clearSettings) {
        const partition = await resetNvs(loader, { tableAddress, partitionName, status: setStatus,
          confirm: entry => window.confirm(writeFirmware
            ? `Clear saved settings in “${entry.name}” and flash the selected firmware?`
            : `Erase saved settings in “${entry.name}”? The installed firmware will be kept.`) });
        if (!partition) {
          resultState = 'reset-cancelled';
          message = 'Cancelled. No settings erased.';
          return;
        }
        cleared = true;
        message = `NVS partition “${partition.name}” erased and verified. Firmware kept.`;
      }
      if (writeFirmware) {
        setStatus('Writing firmware…');
        const total = plan.files.reduce((sum, file) => sum + file.data.length, 0);
        const before = plan.files.map((_, index) => plan.files.slice(0, index).reduce((sum, file) => sum + file.data.length, 0));
        await loader.writeFlash({
          fileArray: plan.files.map(file => ({ data: file.data, address: file.address })),
          flashMode: plan.flashMode, flashFreq: plan.flashFreq, flashSize: plan.flashSize,
          eraseAll: false, compress: true, calculateMD5Hash: md5,
          reportProgress: (index, written, size) => setProgress((before[index] + (size ? written / size : 1) * firmware.files[index].data.length) / total),
        });
        setProgress(1);
        message = cleared ? 'Settings cleared. Firmware written and verified.' : 'Firmware written and verified.';
      }
      resultState = writeFirmware ? 'done' : 'reset-done';
    } catch (error) {
      const detail = error?.message || String(error);
      message = cleared ? `Settings were already cleared. ${detail}` : detail;
      if (/Failed to open serial port|already open|Access denied|NetworkError/i.test(detail)) {
        message += ' Close other programs using this port and try again.';
      } else if (!connected && detail.includes('Failed to connect with the device')) {
        const sdTip = boardId === 'esp32_ai-thinker_cam'
          ? ' If a microSD card is inserted, power off the board and remove it before retrying.'
          : '';
        message = `Could not reach the ESP32 bootloader.${sdTip} Hold BOOT, tap RESET, then release BOOT. Select Manual bootloader under Connection and retry.`;
      }
    } finally {
      if (connected) {
        setStatus('Restarting the board…');
        try {
          await loader.after(resultState === 'done' ? plan.after : 'hard_reset');
          if (resultState === 'done' || resultState === 'reset-done') message += ' Board restarted.';
        } catch {
          message += ' Press RESET to restart the board.';
        }
      }
      if (partial) push(partial);
      try { await transport?.disconnect(); } catch { /* The port may already be closed. */ }
      setState(resultState); setStatus(message);
      if (resultState === 'done' && (firmware.upload ? firmware.firmware : offered)?.provisioning) setShowUsb(true);
    }
  }

  const portInfo = port?.getInfo() ?? {};
  const ready = supported && port && !busy && (writeFirmware ? firmware : clearSettings);
  const selectedBoard = boards.find(board => board.id === boardId);
  const info = boardId && packages?.[project]?.[boardId];
  const upload = firmware?.upload;
  const next = upload ? firmware.firmware : offered;
  let actionLabel = 'Choose an action';
  if (writeFirmware) actionLabel = upload?.kind === 'full' ? 'Flash firmware' : 'Flash firmware (keep settings)';
  if (clearSettings) actionLabel = writeFirmware ? 'Clear settings and flash' : 'Clear settings';

  return <div className="rtc-theme sensorfan"><div className="rtc-body"><SiteHeader project={{ id: 'flasher', name: 'Flasher' }} /><main id="main-content" tabIndex="-1" className="rtc-content flasher">
    <div className="sensorfan-intro"><h1>Flash firmware</h1><p>Flash firmware or reset NVS settings over USB, entirely in this browser. To flash, choose a board or upload your own firmware file. Firmware goes only from this page to the board; it is never sent to a server.</p></div>
    {!supported && <div className="rtc-panel flasher-warning" role="alert"><h2>This browser cannot flash boards</h2><p>Flashing uses Web Serial. Open this page in Chrome or Edge on a computer, over HTTPS or localhost.</p></div>}

    <section className="rtc-panel flasher-step" aria-labelledby="firmware-title">
      <div className="panel-title"><span className="flasher-number">1</span><h2 id="firmware-title">Firmware</h2></div>
      <div className="flasher-source"><label htmlFor="firmware-source">Firmware source</label><select id="firmware-source" disabled={busy} value={project} onChange={event => { setProject(event.target.value); window.history.replaceState(null, '', `#/tools/flasher?project=${event.target.value}`); setBoardId(''); load.current++; setFirmware(null); setFirmwareError(''); setState('idle'); setStatus(''); }}>{firmwares.map(item => <option key={item.id} value={item.id}>{item.name}</option>)}<option value="upload">Upload your firmware</option></select></div>
      {offered && <><p>{offered.description} Choose your board below.</p>
      <fieldset className="flasher-boards" disabled={busy}><legend className="flasher-hidden">Board</legend>{offeredBoards.map(board => {
        const available = packages?.[project]?.[board.id];
        return <label key={board.id} className="flasher-board" data-available={available ? 'true' : 'false'}>
          <input type="radio" name="board" value={board.id} checked={boardId === board.id} disabled={!available} onChange={() => selectBoard(board)} />
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
        <dl>{selectedBoard && <div><dt>Board</dt><dd>{selectedBoard.name}</dd></div>}{upload && <div><dt>File</dt><dd>{firmware.label}</dd></div>}<div><dt>Chip</dt><dd>{chipName(firmware.chip)}</dd></div>{upload && <div><dt>Type</dt><dd>{upload.kind === 'app' ? `App image${upload.project ? ` · ${upload.project}` : ''}${upload.version ? ` ${upload.version}` : ''}` : 'Full flash image'}</dd></div>}{info && <div><dt>Built</dt><dd>{builtDate(info.built)} · {info.local ? 'Local build' : info.commit}</dd></div>}<div><dt>Flash</dt><dd>{upload ? "Board's current settings" : `${firmware.flashSize} · ${firmware.flashMode.toUpperCase()} · ${firmware.flashFreq}`}</dd></div></dl>
        {upload?.kind === 'app' && <p className="flasher-note">The board must already have a bootloader and partition table with an app at 0x{upload.address.toString(16)}, for example from flashing one of the boards above once.</p>}
        {info && !info.local && <a className="button rtc-secondary" href={`firmware/${project}/${boardId}/firmware.bin`} download={`${project}-${boardId}.bin`}>Download firmware file</a>}
        {upload?.kind === 'full' && <p className="flasher-note">A complete flash image replaces existing firmware and settings, including saved Wi-Fi credentials.</p>}
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
        <select id="flash-baud" value={connectionMode === 'manual' ? 115200 : baud} disabled={busy || connectionMode === 'manual'} onChange={event => setBaud(Number(event.target.value))}>{bauds.map(value => <option key={value} value={value}>{value.toLocaleString()} baud</option>)}</select>
        <label htmlFor="flash-connection">Connection</label>
        <select id="flash-connection" value={connectionMode} disabled={busy} onChange={event => setConnectionMode(event.target.value)}>
          <option value="automatic">Automatic reset</option>
          <option value="manual">Manual bootloader</option>
        </select>
        <label className="flasher-check"><input type="checkbox" checked={clearSettings} disabled={busy} onChange={event => setClearSettings(event.target.checked)} /><span>Clear settings <small>Erase saved settings in NVS.</small></span></label>
        <label className="flasher-check"><input type="checkbox" checked={writeFirmware} disabled={busy} onChange={event => setWriteFirmware(event.target.checked)} /><span>Flash firmware</span></label>
      </div>
      {clearSettings && (
        <details className="flasher-reset-options"><summary>Advanced options</summary>
          <label>Partition table address<input value={tableAddress} disabled={busy} spellCheck={false} autoComplete="off" onChange={event => setTableAddress(event.target.value)} /></label>
          <label>NVS partition name (optional)<input value={partitionName} disabled={busy} maxLength={16} placeholder="Auto-detect" autoComplete="off" onChange={event => setPartitionName(event.target.value)} /></label>
        </details>
      )}
      <button type="button" className="button rtc-primary flasher-go" disabled={!ready} onClick={applyFlashActions}>{flashBusy ? 'Working…' : actionLabel}</button>
      {supported && next?.provisioning && <button type="button" className="button rtc-secondary flasher-provision" disabled={!port || busy} onClick={() => setShowUsb(true)}>Provision</button>}
      {state !== 'idle' && !resetMode && <div className="flasher-progress" data-state={state}><progress max="1" value={progress} aria-label="Flashing progress" /><span>{Math.round(progress * 100)}%</span></div>}
      <p role="status" className="flasher-status" data-testid="flash-status">{status}</p>
      {viewerUrl && <p className="flasher-viewer">Camera on Wi-Fi: <a href={viewerUrl} target="_blank" rel="noreferrer">{viewerUrl}</a></p>}
      {state === 'done' && !next?.provisioning && next?.afterFlash.text && <p>{next.afterFlash.text} {next.afterFlash.link && <a href={next.afterFlash.link}>Open {next.name}</a>}</p>}
      {log.length > 0 && <details className="flasher-log"><summary>Details</summary><pre>{log.join('\n')}</pre></details>}
      {connectionMode === 'manual' && <p className="flasher-hint">Hold BOOT, tap RESET, then release BOOT before starting. If your ESP32-CAM has no BOOT button, connect GPIO0 to GND before resetting, then remove the connection after the action finishes and reset again.</p>}
    </section>
    {showUsb && next?.provisioning && <UsbProvisioning port={port} project={next} onClose={() => setShowUsb(false)} onViewer={setViewerUrl} />}
  </main></div></div>;
}
