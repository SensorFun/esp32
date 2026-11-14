import { useState } from 'react';
import { BrandMark, Icon } from '../../components/Icon';
import './webrtc.css';
import { RemoteCamera } from './RemoteCamera';

function LocalCameraLink() {
  const [address, setAddress] = useState('');
  const [error, setError] = useState('');
  function open(event) {
    event.preventDefault();
    try {
      const url = new URL(address);
      if (url.protocol !== 'https:' || url.username || url.password || url.pathname !== '/' || url.search || url.hash) throw new Error();
      setError('');
      window.open(url.href, '_blank', 'noopener,noreferrer');
    } catch { setError('Enter the camera’s HTTPS address, with no credentials or extra path.'); }
  }
  return <form onSubmit={open} className="local-form"><label htmlFor="camera-address">Camera address</label><div><input id="camera-address" type="url" required placeholder="https://device-ip/" value={address} onChange={e => setAddress(e.target.value)} aria-describedby="address-help" /><button className="button rtc-primary" type="submit">Open local viewer <Icon name="external" size={16} /></button></div><p id="address-help" className={error ? 'form-error' : 'muted'} role={error ? 'alert' : undefined}>{error || 'Use the address printed by your board. Opens in a new tab on the same local network.'}</p></form>;
}


function Workspace({ onSessionState }) {
  return <><div className="rtc-title"><div><p className="eyebrow">PROJECT SPACE / 01</p><h1>A window into<br /><span>your hardware.</span></h1><p>Explore live cameras and fine-tune the way they see.</p></div><span className="workspace-label"><Icon name="camera" size={17} /> Camera workspace</span></div><RemoteCamera onSessionState={onSessionState} /><div className="rtc-lower"><section className="rtc-panel"><div className="panel-title"><Icon name="signal" /><h2>Bring a camera online</h2></div><p>The board’s local viewer is ready to use after Wi-Fi provisioning. Open it directly to view video and change camera settings.</p><LocalCameraLink /></section><aside className="rtc-panel connection-note"><span className="pill amber">PRIVATE SIGNALING</span><h2>Local first.<br />Ready for the next step.</h2><p>Connect to your configured camera with its MAC address and camera password. Your private signaling service handles the connection automatically.</p><a href="#/spaces/webrtc/setup" className="text-link">See the connection guide <Icon name="arrow" size={17} /></a></aside></div></>;
}

function Setup() {
  return <><div className="rtc-title"><div><p className="eyebrow">GET CONNECTED</p><h1>From your board<br /><span>to your browser.</span></h1><p>View locally or connect remotely with your camera MAC and password.</p></div></div><div className="setup-grid"><section className="rtc-panel"><span className="pill mint">AVAILABLE ON THE BOARD</span><h2>Local connection</h2><ol className="steps"><li><span>01</span><div><h3>Provision your camera</h3><p>Connect the ESP32 to Wi-Fi using its setup page or the USB provisioning tool.</p></div></li><li><span>02</span><div><h3>Find its address</h3><p>The USB console prints the local HTTPS address when the camera is ready.</p></div></li><li><span>03</span><div><h3>Open the local viewer</h3><p>Use a browser on the same network. Accept the device certificate notice and click Connect. No pairing code is required.</p></div></li></ol><LocalCameraLink /></section><section className="rtc-panel"><span className="pill amber">PRIVATE SIGNALING</span><h2>Internet connection</h2><p>Once your camera is configured, open Workspace and enter its MAC address and camera password. The site connects to its configured signaling service automatically.</p><ul className="requirements"><li><Icon name="signal" /><div><h3>Automatic signaling</h3><p>Janus forwards connection messages through Cloudflare Tunnel. Both peers derive a private room from the camera MAC and password.</p></div></li><li><Icon name="grid" /><div><h3>STUN discovery</h3><p>Find a direct route for encrypted traffic. TURN relaying is disabled; restrictive networks can prevent a connection.</p></div></li><li><Icon name="chip" /><div><h3>Password protection</h3><p>Choose a camera password during provisioning. The ESP32 checks it before allowing video or controls.</p></div></li></ul><p className="inline-note">GitHub Pages hosts the interface. Janus runs on your computer through Cloudflare Tunnel; TURN is disabled.</p></section></div></>;
}

function Notes() {
  return <><div className="rtc-title"><div><p className="eyebrow">INSIDE THIS SPACE</p><h1>Real devices.<br /><span>Real-time possibilities.</span></h1><p>A dedicated home for the WebRTC camera project.</p></div></div><div className="notes-grid"><section className="rtc-panel"><Icon name="camera" size={26} /><h2>Two camera platforms</h2><p>The firmware supports ESP32-CAM with OV2640 and ESP32-S3 with OV3660. The detected sensor determines the available settings.</p></section><section className="rtc-panel"><Icon name="settings" size={26} /><h2>A different point of view</h2><p>The board viewer exposes exposure, contrast, white balance, orientation, and High, Mid, and Low light timing profiles.</p></section><section className="rtc-panel"><Icon name="signal" size={26} /><h2>Encrypted connections</h2><p>Camera images and setting commands travel over the WebRTC data channel. HTTPS handles the local page and connection setup.</p></section></div><div className="space-note"><span>PART OF DEVICE SPACES</span><p>This project has its own visual identity and navigation. Return to the main collection whenever you want to explore another space.</p><a href="#/spaces" className="button rtc-secondary">All project spaces <Icon name="arrow" /></a></div></>;
}

export function WebRTCSpace({ subpage = '' }) {
  const [sessionState, setSessionState] = useState('idle');
  const tabs = [{ path: '', label: 'Workspace', icon: 'camera' }, { path: 'setup', label: 'Connection guide', icon: 'signal' }, { path: 'notes', label: 'About this space', icon: 'book' }];
  const tab = tabs.find(t => t.path === subpage);
  return <div className="rtc-theme"><aside className="rtc-sidebar"><a href="#/" className="back-to-home"><Icon name="back" size={16} /> Device Spaces</a><a href="#/spaces/webrtc" className="rtc-brand"><span><Icon name="camera" size={26} /></span><div>WebRTC<small>CAMERA LAB</small></div></a><p className="sidebar-caption">PROJECT NAVIGATION</p><nav aria-label="WebRTC navigation">{tabs.map(t => <a key={t.path} href={`#/spaces/webrtc${t.path ? '/' + t.path : ''}`} aria-current={subpage === t.path ? 'page' : undefined}><Icon name={t.icon} size={18} />{t.label}{subpage === t.path && <span className="nav-dot" />}</a>)}</nav><div className="sidebar-bottom"><div><BrandMark /><span>A Device Spaces project</span></div><a href="#/spaces">Explore all spaces <Icon name="arrow" size={15} /></a></div></aside><div className="rtc-body"><header className="rtc-header"><div><a href="#/spaces">Spaces</a><span>/</span><a href="#/spaces/webrtc">WebRTC</a><span>/</span><span>{tab?.label || 'Page not found'}</span></div><span className="pill"><span className="status-dot idle" /> {sessionState === 'connected' ? 'Camera connected' : sessionState === 'connecting' ? 'Connecting…' : 'No active session'}</span></header><main id="main-content" tabIndex="-1" className="rtc-content">{!tab ? <section className="rtc-title"><div><h1>Page not found.</h1><a className="button rtc-primary" href="#/spaces/webrtc">Return to workspace</a></div></section> : subpage === 'setup' ? <Setup /> : subpage === 'notes' ? <Notes /> : <Workspace onSessionState={setSessionState} />}</main><footer className="rtc-footer"><span>WEBRTC / CAMERA LAB</span><span>Built for a closer look.</span></footer></div></div>;
}
