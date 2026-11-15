import { WebRTCProject } from './webrtc/WebRTCProject';

// Each project owns its layout and styles. Add new projects here.
export const projects = [{
  id: 'webrtc', name: 'WebRTC', category: 'Connected cameras', icon: 'camera',
  description: 'A dedicated workspace for real-time cameras, device controls, and encrypted connections.',
  component: WebRTCProject,
}];
