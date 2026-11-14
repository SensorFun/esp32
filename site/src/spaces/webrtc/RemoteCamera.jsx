import { useEffect, useRef, useState } from 'react';
import { createProof, deriveCameraAccess, verifyServer } from './access.mjs';
import { JanusSignaling } from './janus-signaling.mjs';
import { stunOnlyServers } from './ice.mjs';
import { Icon } from '../../components/Icon';

export function RemoteCamera({ onSessionState }) {
  const [camera, setCamera] = useState('');
  const [password, setPassword] = useState('');
  const [status, setStatus] = useState('Enter your camera MAC address and provisioning password.');
  const [state, setState] = useState('idle');
  const [settings, setSettings] = useState(null);
  const [transport, setTransport] = useState('—');
  const image = useRef(null);
  const connection = useRef(null);
  useEffect(() => { onSessionState(state); }, [state, onSessionState]);
  const generation = useRef(0);
  function stop(message = 'Disconnected') {
    generation.current++;
    const session = connection.current;
    connection.current = null;
    if (session) {
      clearInterval(session.heartbeat); clearTimeout(session.timeout);
      session.socket?.close(); session.channel?.close(); session.peer?.close();
      if (session.url) URL.revokeObjectURL(session.url);
    }
    setState('idle'); setSettings(null); setTransport('—'); setStatus(message);
    if (image.current) image.current.removeAttribute('src');
  }
  useEffect(() => () => { generation.current++; onSessionState('idle'); const session = connection.current; if (session) { clearInterval(session.heartbeat); clearTimeout(session.timeout); session.socket?.close(); session.peer?.close(); if (session.url) URL.revokeObjectURL(session.url); } }, []);
  function command(message) { const channel = connection.current?.channel; if (connection.current?.authenticated && channel?.readyState === 'open') channel.send(JSON.stringify(message)); }
  async function connect() {
    const accessPassword = password;
    stop();
    const current = generation.current;
    const alive = () => current === generation.current;
    setState('connecting'); setStatus('Finding private camera room…');
    let access;
    try { access = await deriveCameraAccess(camera, accessPassword); }
    catch (error) { if (alive()) stop(error.message); return; }
    if (!alive()) return;
    let socket;
    try {
      socket = new JanusSignaling(access.room, import.meta.env.VITE_SIGNALING_URL);
    } catch (error) { if (alive()) stop(error.message); return; }
    const session = { socket, peer: null, channel: null, frame: null, frameId: -1, received: 0, url: null, candidates: [] };
    connection.current = session;
    session.timeout = setTimeout(() => { if (alive()) stop('Camera not found. Check the MAC/password and camera internet connection.'); }, 15000);
    async function data(event) {
      if (!alive()) return;
      if (typeof event.data === 'string') {
        try {
          const message = JSON.parse(event.data);
          if (message.type === 'auth_challenge') {
            if (session.authStarted) throw Error('Repeated authentication challenge');
            session.authStarted = true;
            const response = await createProof(accessPassword, access.id, message, session.peer.localDescription.sdp, session.peer.remoteDescription.sdp, access);
            if (!alive()) return;
            session.expectedSignature = response.expected;
            session.channel.send(JSON.stringify({ op: 'authenticate', proof: response.proof }));
          } else if (message.type === 'auth_ok') {
            if (session.authenticated || !verifyServer(message.signature, session.expectedSignature)) throw Error('Camera identity verification failed');
            session.authenticated = true; session.expectedSignature = null;
            startAuthenticated();
          } else if (message.type === 'auth_error') throw Error(message.error || 'Camera authentication failed');
          else if (session.authenticated && message.type === 'settings') setSettings(message);
          else if (session.authenticated && message.type === 'error') setStatus(message.error);
        } catch (error) { if (alive()) stop(error.message); }
        return;
      }
      if (!session.authenticated || !(event.data instanceof ArrayBuffer) || event.data.byteLength < 16) return;
      const view = new DataView(event.data);
      if (view.getUint32(0, true) !== 0x4745504a) return;
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
            command({ op: 'get' });
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
            if (!candidate || !alive()) return;
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
        } else if (message.type === 'bye') stop('Camera ended the session');
      } catch (error) { if (alive()) stop(error.message); }
    };
    session.socket.onerror = error => { if (alive()) stop(error.message || 'Cannot reach the signaling server.'); };
    session.socket.onclose = () => { if (alive() && connection.current) stop('Signaling connection disconnected'); };
  }
  return <section className="remote-camera" data-state={state}>
    <div className="rtc-panel remote-connection"><div className="panel-title"><Icon name="signal" /><h2>Remote camera</h2></div><p>Enter the camera MAC and the password chosen during setup. Your camera room is derived privately from both.</p>
      <div className="remote-credentials">
        <label htmlFor="camera-mac">Camera MAC address</label><input id="camera-mac" value={camera} placeholder="AA:BB:CC:DD:EE:FF" autoComplete="off" disabled={state !== 'idle'} onChange={e => setCamera(e.target.value)} /><label htmlFor="camera-access-password">Camera password</label><input id="camera-access-password" type="password" autoComplete="current-password" value={password} disabled={state !== 'idle'} onChange={e => setPassword(e.target.value)} /><div className="remote-actions"><button className="button rtc-primary" disabled={!password || !camera || state !== 'idle'} onClick={connect}>Connect camera</button><button className="button rtc-secondary" disabled={state === 'idle'} onClick={() => stop()}>Disconnect</button></div></div>
      <p className="muted">Direct WebRTC · STUN enabled · TURN relay disabled</p><p role="status" data-testid="remote-status">{status}</p>
    </div>
    <div className="monitor"><div className="monitor-top"><span>CAMERA PREVIEW</span><span>{transport}</span></div><div className="remote-preview"><img ref={image} alt="Remote camera stream" data-testid="remote-image" />{state !== 'connected' && <span>Awaiting encrypted camera stream</span>}</div></div>
    {settings && <section className="rtc-panel remote-settings"><h2>Camera settings · {settings.sensor}</h2><label>Light timing<select aria-label="Light timing" value={settings.timing_profile} onChange={e => command({ op: 'set', name: 'timing_profile', value: e.target.value })}>{settings.timing_profiles.map(name => <option key={name} value={name}>{({ high_light: 'High light', balanced: 'Mid light', low_light: 'Low light' })[name]}</option>)}</select></label><div className="remote-controls">{settings.controls.map(c => <label key={c.name}>{c.name}<input aria-label={c.name} type="number" min={c.min} max={c.max} step="1" value={c.value} disabled={!c.supported} onChange={e => { const value = Number(e.target.value); if (Number.isInteger(value) && value >= c.min && value <= c.max) command({ op: 'set', name: c.name, value }); }} /></label>)}</div></section>}
  </section>;
}
