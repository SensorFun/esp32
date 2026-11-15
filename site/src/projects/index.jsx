import { WebRTCProject } from './webrtc/WebRTCProject';
import { FlasherProject } from './flasher/FlasherProject';

// Each project owns its layout and styles. Add new projects here.
export const projects = [{
  id: 'webrtc', name: 'WebRTC', category: 'Connected cameras', icon: 'camera',
  description: 'A dedicated workspace for real-time cameras, device controls, and encrypted connections.',
  component: WebRTCProject,
}, {
  id: 'flasher', name: 'Flasher', category: 'Tools', icon: 'chip',
  description: 'Flash firmware you built to an ESP32 board over USB, right from the browser.',
  component: FlasherProject,
}];
