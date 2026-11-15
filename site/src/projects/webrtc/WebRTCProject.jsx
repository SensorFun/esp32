import { useState } from 'react';
import './webrtc.css';
import { RemoteCamera } from './RemoteCamera';

export function WebRTCProject() {
  const [sessionState, setSessionState] = useState('idle');
  // The project workspace remains separate from the SensorFan project index.
  return <div className="rtc-theme sensorfan"><div className="rtc-body"><header className="sensorfan-header"><a href="#/" className="sensorfan-brand"><img src="./logo.png" alt="" /><span>SensorFan</span></a><span className="pill">{sessionState === 'connected' ? 'Camera connected' : sessionState === 'connecting' ? 'Connecting…' : 'Ready to connect'}</span></header><main id="main-content" tabIndex="-1" className="rtc-content"><RemoteCamera onSessionState={setSessionState} /></main></div></div>;
}
