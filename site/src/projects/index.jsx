import { WebRTCProject } from './webrtc/WebRTCProject';
import { FlasherProject } from './flasher/FlasherProject';
import { projects as projectData, tools as toolData } from './catalog.mjs';

// Each project owns its layout and styles. Add new projects to catalog.mjs and here.
const components = { webrtc: WebRTCProject, flasher: FlasherProject };
export const projects = projectData.map(item => ({ ...item, component: components[item.id] }));
export const tools = toolData.map(item => ({ ...item, component: components[item.id] }));
