import { existsSync, readFileSync, realpathSync, statSync } from 'node:fs';
import path from 'node:path';
import { readCatalog, readFirmwareCatalog } from './boards-plugin.mjs';

function fileInside(folder, name) {
  if (!name || !name.split('/').every(part => /^[\w.-]+$/.test(part) && part !== '..' && part !== '.')) {
    throw Error('Invalid firmware path');
  }
  const file = realpathSync(path.join(folder, name));
  if (!file.startsWith(folder + path.sep) || !statSync(file).isFile()) throw Error('Invalid firmware file');
  return file;
}

function localBuild(root, project, board) {
  try {
    const folder = realpathSync(path.join(root, project, board.id));
    if (!folder.startsWith(root + path.sep)) return null;
    const argsFile = fileInside(folder, 'flasher_args.json');
    const selectionFile = fileInside(folder, 'firmware_selection.json');
    const args = JSON.parse(readFileSync(argsFile, 'utf8'));
    const selection = JSON.parse(readFileSync(selectionFile, 'utf8'));
    if (selection.board !== board.id || args.extra_esptool_args?.chip !== board.target) return null;
    const files = new Map([
      ['flasher_args.json', argsFile],
      ['firmware_selection.json', selectionFile],
    ]);
    const segments = Object.values(args.flash_files);
    if (!segments.length) return null;
    for (const name of segments) {
      if (path.extname(name) !== '.bin') return null;
      files.set(name, fileInside(folder, name));
    }
    const app = args.app;
    const binary = files.get(app?.file);
    if (!binary || args.flash_files[app.offset] !== app.file) return null;
    return { files, info: {
      local: true,
      built: Math.floor(statSync(binary).mtimeMs / 1000),
      frame_grabber: selection.frame_grabber,
      app_offset: app.offset,
    } };
  } catch { return null; }
}

// Serve raw ESP-IDF output directly from disk. The production plugin keeps
// using packaged firmware from CI and never reads this override.
export function localFirmwarePlugin(workspace, root) {
  return {
    name: 'local-firmware',
    apply: 'serve',
    enforce: 'pre',
    configureServer(server) {
      server.middlewares.use('/firmware/', (request, response) => {
        const send = (status, body, type = 'application/json') => {
          response.statusCode = status;
          response.setHeader('Content-Type', type);
          response.setHeader('Cache-Control', 'no-store');
          response.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
          response.end(request.method === 'HEAD' ? undefined : body);
        };
        const host = request.headers.host;
        if (!/^(127\.0\.0\.1|localhost)(:\d+)?$/.test(host ?? '') ||
            (request.headers.origin && request.headers.origin !== `http://${host}`) ||
            request.headers['sec-fetch-site'] === 'cross-site') return send(403, '{"error":"Local access only"}');
        if (!['GET', 'HEAD'].includes(request.method)) return send(405, '{"error":"Method not allowed"}');

        const resource = request.url.split('?')[0].replace(/^\/+/, '');
        if (!existsSync(root)) return resource === 'index.json'
          ? send(200, '{}')
          : send(404, '{"error":"Firmware file not found"}');
        const buildRoot = realpathSync(root);
        const boards = readCatalog(workspace);
        const projects = readFirmwareCatalog(workspace, boards);
        if (resource === 'index.json') {
          const manifest = {};
          for (const project of projects) {
            for (const board of boards.filter(board => project.boards.includes(board.id))) {
              const build = localBuild(buildRoot, project.id, board);
              if (build) (manifest[project.id] ??= {})[board.id] = build.info;
            }
          }
          return send(200, JSON.stringify(manifest));
        }
        const [projectId, boardId, ...parts] = resource.split('/');
        const board = boards.find(board => board.id === boardId);
        const project = projects.find(project => project.id === projectId && project.boards.includes(boardId));
        const name = parts.join('/');
        const file = board && project && localBuild(buildRoot, project.id, board)?.files.get(name);
        if (!file) return send(404, '{"error":"Firmware file not found"}');
        try {
          return send(200, readFileSync(file), name.endsWith('.bin') ? 'application/octet-stream' : 'application/json');
        } catch { return send(404, '{"error":"Firmware file not found"}'); }
      });
    },
  };
}
