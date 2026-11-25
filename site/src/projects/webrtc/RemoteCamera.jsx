import { useEffect, useRef, useState } from 'react';
import { createProof, deriveCameraAccess, normalizeMac, verifyServer } from './access.mjs';
import { Signaling } from './signaling.mjs';
import { base, currentRoute } from '../../routes.mjs';
import { stunOnlyServers } from './ice.mjs';
import { Icon } from '../../components/Icon';
import '../../../../projects/common/camera_ui/settings.js';
import '../../../../projects/common/camera_ui/logs.js';
import '../../../../projects/common/camera_ui/motion.js';

const cameraCookie = 'esp32_camera_mac';
const formatMac = value => normalizeMac(value).match(/../g).join(':').toUpperCase();
function savedCamera() {
  try {
    const value = document.cookie.split('; ').find(cookie => cookie.startsWith(cameraCookie + '='));
    return value ? formatMac(decodeURIComponent(value.slice(cameraCookie.length + 1))) : '';
  } catch { return ''; }
}
// The camera from a link such as /projects/webrtc/A1B2C3D4E5F6 (shown after Wi-Fi setup).
function linkedCamera() {
  const match = currentRoute().match(/^\/projects\/webrtc\/([^/]+)\/?$/);
  try { return match ? formatMac(decodeURIComponent(match[1])) : ''; } catch { return ''; }
}
function rememberCamera(value) {
  try {
    const mac = value.trim() ? formatMac(value) : '';
    const path = base;  // Site-wide, as when every page lived at the base with a #route.
    document.cookie = `${cameraCookie}=${encodeURIComponent(mac)}; Path=${path}; Max-Age=${mac ? 31536000 : 0}; SameSite=Lax${window.location.protocol === 'https:' ? '; Secure' : ''}`;
  } catch { /* Invalid input or blocked cookies must not prevent camera access. */ }
}

export function RemoteCamera({ onSessionState }) {
  const [camera, setCamera] = useState(() => linkedCamera() || savedCamera());
  useEffect(() => { rememberCamera(camera); }, [camera]);
  const [password, setPassword] = useState('');
  const [status, setStatus] = useState('');
  const [state, setState] = useState('idle');
  const [settings, setSettings] = useState(null);
  const [transport, setTransport] = useState('—');
  const [fps, setFps] = useState(0);
  const image = useRef(null);
  const connection = useRef(null);
  // Only a non-extractable key derived from the password is kept, so Connect
  // can reconnect; the typed password is cleared as soon as Connect starts.
  // The camera, not this page, checks the proof made with that key.
  const savedAccess = useRef(null);
  const settingsPanel = useRef(null);
  const root = useRef(null);
  const motionPanel = useRef(null);
  const recordingPanel = useRef(null);
  const recordings = useRef(null);
  const overlay = useRef(null);
  const logs = useRef(null);
  const motionViews = () => [motionPanel.current, recordingPanel.current, recordings.current, overlay.current, logs.current].filter(Boolean);
  useEffect(() => { if (settings && settingsPanel.current) settingsPanel.current.settings = settings; }, [settings]);
  useEffect(() => {
    // Camera, motion, and recording panels all request commands with this event.
    const element = root.current;
    const send = event => command(event.detail);
    element.addEventListener('camera-command', send);
    return () => element.removeEventListener('camera-command', send);
  }, []);
  useEffect(() => { onSessionState(state); }, [state, onSessionState]);
  const generation = useRef(0);
  function stop(message = 'Disconnected') {
    generation.current++;
    const session = connection.current;
    connection.current = null;
    if (session) {
      clearInterval(session.heartbeat); clearTimeout(session.timeout);
      session.socket?.close({ notifyPeer: Boolean(session.peer) }); session.channel?.close(); session.peer?.close();
      if (session.url) URL.revokeObjectURL(session.url);
    }
    logs.current?.recordBrowser(message);
    setState('idle'); setSettings(null); setTransport('—'); setFps(0); setStatus(message);
    for (const view of motionViews()) view.reset();
    if (image.current) image.current.removeAttribute('src');
  }
  useEffect(() => () => { generation.current++; onSessionState('idle'); const session = connection.current; if (session) { clearInterval(session.heartbeat); clearTimeout(session.timeout); session.socket?.close({ notifyPeer: Boolean(session.peer) }); session.peer?.close(); if (session.url) URL.revokeObjectURL(session.url); } }, []);
  function command(message) { const channel = connection.current?.channel; if (connection.current?.authenticated && channel?.readyState === 'open') channel.send(JSON.stringify(message)); }
  async function connect() {
    let secret = password;
    setPassword('');
    stop();
    const current = generation.current;
    const alive = () => current === generation.current;
    setState('connecting'); setStatus('Finding private camera room…');
    let access;
    try {
      access = secret ? await deriveCameraAccess(camera, secret) : savedAccess.current;
      secret = '';
      if (!access) throw Error('Enter the camera password.');
      savedAccess.current = access;
    } catch (error) { if (alive()) stop(error.message); return; }
    if (!alive()) return;
    let socket;
    try {
      socket = new Signaling(access.room, 'wss://signalserver.sensorfun.net/viewer');
    } catch (error) { if (alive()) stop(error.message); return; }
    const session = { socket, peer: null, channel: null, frame: null, frameId: -1, received: 0, url: null, candidates: [] };
    connection.current = session;
    session.timeout = setTimeout(() => { if (alive()) { savedAccess.current = null; stop('Camera not found. Check the MAC/password and camera internet connection.'); } }, 15000);
    async function data(event) {
      if (!alive()) return;
      if (typeof event.data === 'string') {
        try {
          const message = JSON.parse(event.data);
          if (message.type === 'auth_challenge') {
            if (session.authStarted) throw Error('Repeated authentication challenge');
            session.authStarted = true;
            const response = await createProof('', access.id, message, session.peer.localDescription.sdp, session.peer.remoteDescription.sdp, access);
            if (!alive()) return;
            session.expectedSignature = response.expected;
            session.channel.send(JSON.stringify({ op: 'authenticate', proof: response.proof }));
          } else if (message.type === 'auth_ok') {
            if (session.authenticated || !verifyServer(message.signature, session.expectedSignature)) throw Error('Camera identity verification failed');
            session.authenticated = true; session.expectedSignature = null;
            if (logs.current) logs.current.connected = true;
            startAuthenticated();
          } else if (message.type === 'auth_error') { savedAccess.current = null; throw Error(message.error || 'Camera authentication failed'); }
          else if (session.authenticated && message.type === 'settings') {
            const disabled = message.stream_images === 0;
            if (disabled && !session.imagesDisabled) {
              session.frame = null;
              image.current?.removeAttribute('src');
              if (session.url) URL.revokeObjectURL(session.url);
              session.url = null;
            }
            session.imagesDisabled = disabled;
            setSettings(message);
          }
          else if (session.authenticated && message.type === 'error') { setStatus(message.error); if (settingsPanel.current) settingsPanel.current.error = message.error; }
          else if (session.authenticated) {
            // The camera's grab rate, whether or not images are streamed.
            if (message.type === 'motion_status') {
              setFps(Number.isFinite(message.capture_fps) ? message.capture_fps : 0);
            }
            for (const view of motionViews()) view.handle(message);
          }
        } catch (error) { if (alive()) stop(error.message); }
        return;
      }
      if (!session.authenticated || !(event.data instanceof ArrayBuffer) || event.data.byteLength < 16) return;
      if (recordings.current?.binary(event.data)) return;
      const view = new DataView(event.data);
      if (session.imagesDisabled || view.getUint32(0, true) !== 0x4745504a) return;
      const id = view.getUint32(4, true), total = view.getUint32(8, true), offset = view.getUint32(12, true), bytes = new Uint8Array(event.data, 16);
      if (!total || total > 2 * 1024 * 1024 || offset + bytes.length > total) return;
      if (!offset) { session.frame = new Uint8Array(total); session.frameId = id; session.received = 0; }
      if (!session.frame || id !== session.frameId || offset !== session.received || total !== session.frame.length) { session.frame = null; return; }
      session.frame.set(bytes, offset); session.received += bytes.length;
      if (session.received === total) {
        const previous = session.url;
        session.url = URL.createObjectURL(new Blob([session.frame], { type: 'image/jpeg' }));
        if (image.current) image.current.src = session.url;
        if (previous) URL.revokeObjectURL(previous);
        session.frame = null;
      }
    }
    function startAuthenticated() {
      if (!alive()) return;
            clearTimeout(session.timeout); setState('connected'); setStatus('Connected · password verified · encrypted WebRTC');
            command({ op: 'time', now: Date.now() });
            command({ op: 'get' });
            command({ op: 'motion_get' });
            command({ op: 'recordings' });
            session.heartbeat = setInterval(async () => {
              command({ op: 'ping' });
              command({ op: 'get' });
              const stats = await session.peer.getStats();
              stats.forEach(report => {
                if (report.type === 'transport' && report.selectedCandidatePairId) {
                  const pair = stats.get(report.selectedCandidatePairId), local = stats.get(pair?.localCandidateId), remote = stats.get(pair?.remoteCandidateId);
                  if (local?.candidateType === 'relay' || remote?.candidateType === 'relay') { if (alive()) stop('Unexpected relay candidate: TURN is disabled.'); }
                  else if (alive()) setTransport('Direct · DTLS');
                }
              });
            }, 5000);
    }
    session.socket.onmessage = async event => {
      if (!alive()) return;
      try {
        const message = JSON.parse(event.data);
        if (message.type === 'bye') { stop('Camera ended the session'); return; }
        // An authenticated peer uses its direct data channel for liveness.
        if (session.authenticated) return;
        if (message.type === 'signaling_lost') { stop('Camera signaling connection interrupted'); return; }
        if (message.type === 'error') throw Error(message.error);
        if (message.type === 'ready') {
          if (session.peer) return;
          clearTimeout(session.timeout);
          session.timeout = setTimeout(() => { if (alive()) stop('Direct connection timed out. TURN is disabled.'); }, 45000);
          const iceServers = stunOnlyServers(message.iceServers);
          if (!iceServers.length) throw Error('No STUN configuration; TURN is disabled.');
          const peer = new RTCPeerConnection({ iceServers, iceTransportPolicy: 'all' });
          session.peer = peer;
          const channel = peer.createDataChannel('camera', { ordered: true });
          session.channel = channel; channel.binaryType = 'arraybuffer'; channel.onmessage = data;
          channel.onopen = () => { if (alive()) setStatus('Verifying camera password…'); };
          channel.onclose = () => { if (alive()) stop('Camera disconnected'); };
          peer.onconnectionstatechange = () => { if (alive() && ['failed', 'closed'].includes(peer.connectionState)) stop('WebRTC connection closed'); };
          let offerSent = false;
          const localCandidates = [];
          peer.onicecandidate = ({ candidate }) => {
            if (!candidate || !alive() || session.authenticated) return;
            const signal = JSON.stringify({ type: 'candidate', candidate: candidate.candidate });
            if (!offerSent) localCandidates.push(signal);
            else {
              try { session.socket.send(signal); }
              catch (error) { if (alive()) stop(error.message); }
            }
          };
          setStatus('Establishing direct camera connection…');
          await peer.setLocalDescription(await peer.createOffer());
          if (!alive()) return;
          session.socket.send(JSON.stringify(peer.localDescription));
          offerSent = true;
          for (const candidate of localCandidates) session.socket.send(candidate);

        } else if (message.type === 'answer') {
          setStatus('Establishing encrypted camera connection…');
          await session.peer.setRemoteDescription({ type: 'answer', sdp: message.sdp });
          for (const candidate of session.candidates.splice(0)) await session.peer.addIceCandidate(candidate);
        } else if (message.type === 'candidate') {
          const candidate = { candidate: message.candidate, sdpMid: '0', sdpMLineIndex: 0 };
          if (session.peer?.remoteDescription) await session.peer.addIceCandidate(candidate);
          else session.candidates.push(candidate);
        }
      } catch (error) { if (alive()) stop(error.message); }
    };
    session.socket.onerror = error => { if (alive() && !session.authenticated) stop(error.message || 'Cannot reach the signaling server.'); };
    session.socket.onclose = () => { if (alive() && !session.authenticated) stop('Signaling connection disconnected'); };
  }
  // While image streaming is off, the placeholder (and the motion cells over it)
  // takes the selected resolution's size, so it matches the stream it replaces.
  const blankSize = settings?.resolutions?.find(size => size.value === settings.resolution) || { width: 640, height: 480 };
  return <section className="remote-camera" data-state={state} ref={root}>
    <div className="rtc-panel remote-connection"><div className="panel-title"><Icon name="signal" /><h2>Remote camera</h2></div>
      <form className="remote-credentials" onSubmit={event => { event.preventDefault(); if (camera && (password || savedAccess.current) && state === 'idle') connect(); }}>
        <label htmlFor="camera-mac">Camera MAC address</label><input id="camera-mac" value={camera} placeholder="AA:BB:CC:DD:EE:FF" autoComplete="off" disabled={state !== 'idle'} onChange={e => { savedAccess.current = null; setCamera(e.target.value); }} /><label htmlFor="camera-access-password">Camera password</label><input id="camera-access-password" type="password" autoComplete="current-password" value={password} disabled={state !== 'idle'} onChange={e => setPassword(e.target.value)} /><div className="remote-actions"><button type="submit" className="button rtc-primary" disabled={(!password && !savedAccess.current) || !camera || state !== 'idle'}>Connect camera</button><button type="button" className="button rtc-secondary" disabled={state === 'idle'} onClick={() => stop()}>Disconnect</button></div></form>
      <p className="muted">Direct WebRTC</p><p role="status" data-testid="remote-status">{status}</p>
    </div>
    <div className="monitor"><div className="monitor-top"><span>CAMERA PREVIEW</span><span data-testid="remote-fps" title="Frames the camera grabs per second for live video, motion detection or recording · 1-second average">{fps.toFixed(1)} FPS</span><span>{transport}</span></div><div className="remote-preview"><div className={settings?.stream_images === 0 ? 'preview-frame blank' : 'preview-frame'}><img ref={image} alt="Remote camera stream" data-testid="remote-image" hidden={settings?.stream_images === 0} />{settings?.stream_images === 0 && <canvas className="blank-frame" width={blankSize.width} height={blankSize.height} data-testid="remote-blank" aria-label="Camera image streaming is off; motion is still shown" />}<sensorfan-motion-overlay ref={overlay} /></div>{state !== 'connected' && <span>Awaiting encrypted camera stream</span>}{settings?.stream_images === 0 && <span role="status">Camera image streaming is off.</span>}</div></div>
    <sensorfan-camera-tabs>
      <div data-label="Camera settings">{settings ? <sensorfan-camera-settings className="remote-settings" ref={settingsPanel} /> : <p className="muted">Connect a camera to adjust its settings.</p>}</div>
      <div data-label="Motion detection" hidden><sensorfan-motion-settings ref={motionPanel} group="detection" hidden data-testid="motion-settings" /></div>
      <div data-label="SD card recording" hidden><sensorfan-motion-settings ref={recordingPanel} group="recording" hidden data-testid="recording-settings" /><sensorfan-recordings ref={recordings} hidden data-testid="recordings" /></div>
    <div data-label="Diagnostics" hidden><sensorfan-camera-logs ref={logs} /></div>
    </sensorfan-camera-tabs>
  </section>;
}
