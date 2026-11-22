// Plain route data, shared by the app (projects/index.jsx adds components) and
// the build, which writes a page for every route (scripts/routes-plugin.mjs).
export const projects = [{
  id: 'webrtc', name: 'WebRTC', category: 'Connected cameras', icon: 'camera',
  // /projects/webrtc/<camera MAC> opens the viewer with that camera filled in.
  subpaths: true,
  description: 'A dedicated workspace for real-time cameras, device controls, and encrypted connections.',
}];

export const tools = [{
  id: 'flasher', name: 'Flasher', category: 'Tools', icon: 'chip',
  description: 'Flash firmware you built to an ESP32 board over USB, right from the browser.',
}];

// Site paths (without the base), e.g. /projects/webrtc.
export const routes = ['/', '/projects', '/tools', ...projects.map(item => `/projects/${item.id}`), ...tools.map(item => `/tools/${item.id}`)];
