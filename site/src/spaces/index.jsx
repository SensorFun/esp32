import { WebRTCSpace } from './webrtc/WebRTCSpace';

// Each space owns its layout, styles, and subpages. Add new projects here.
export const spaces = [{
  id: 'webrtc', name: 'WebRTC', category: 'Connected cameras', icon: 'camera',
  description: 'A dedicated workspace for real-time cameras, device controls, and encrypted connections.',
  component: WebRTCSpace,
}];
