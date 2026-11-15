import { useState } from 'react';
import './webrtc.css';
import { RemoteCamera } from './RemoteCamera';
import { SiteHeader } from '../../components/SiteHeader';

export function WebRTCProject() {
  const [sessionState, setSessionState] = useState('idle');
  return <div className="rtc-theme sensorfan"><div className="rtc-body"><SiteHeader project={{ id: 'webrtc', name: 'WebRTC' }}><span className="pill sensorfan-status">{sessionState === 'connected' ? 'Camera connected' : sessionState === 'connecting' ? 'Connecting…' : 'Ready to connect'}</span></SiteHeader><main id="main-content" tabIndex="-1" className="rtc-content"><RemoteCamera onSessionState={setSessionState} /></main></div></div>;
}
