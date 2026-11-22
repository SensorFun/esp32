// Vite plugin: board catalog (config/boards/*.yaml), flashable firmware catalog
// (config/firmware/*.yaml), and packages built by tools/firmware_artifacts.py in
// build/firmware/<project>/<board>/. Python's esp32_workspace.firmware_catalog
// is the authoritative validator; this mirrors its board resolution.
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import path from 'node:path';
import { parse } from 'yaml';

const virtualId = 'virtual:board-catalog';
const resolvedId = '\0' + virtualId;
// One module per project's provisioning page, so each page is its own chunk.
// (A plain `page.html?raw` import is served as HTML by the dev server.)
const pagePrefix = 'virtual:provisioning-page/';
const idPattern = /^[A-Za-z0-9_-]+$/;
const types = { '.json': 'application/json', '.bin': 'application/octet-stream' };

export function readCatalog(workspace) {
  const folder = path.join(workspace, 'config/boards');
  const boards = [];
  for (const file of readdirSync(folder).filter(name => name.endsWith('.yaml')).sort()) {
    const entries = parse(readFileSync(path.join(folder, file), 'utf8')) ?? {};
    for (const [id, entry] of Object.entries(entries)) {
      if (!idPattern.test(id) || typeof entry?.name !== 'string' || typeof entry?.target !== 'string') throw Error(`Invalid board entry ${id} in ${file}`);
      let image = null;
      if (entry.image !== undefined) {
        image = path.resolve(folder, entry.image);
        if (!image.startsWith(path.join(folder, 'images') + path.sep) || !existsSync(image)) throw Error(`Missing or invalid image for ${id}`);
      }
      const grabbers = Array.isArray(entry.supported_frame_grabber) ? entry.supported_frame_grabber : [];
      boards.push({ id, name: entry.name, target: entry.target, image, grabbers, file: path.join(folder, file) });
    }
  }
  return boards.sort((a, b) => a.name.localeCompare(b.name));
}

// Flashable projects with their resolved boards, as Python's build_pairs lists them.
export function readFirmwareCatalog(workspace, boards = readCatalog(workspace)) {
  const folder = path.join(workspace, 'config/firmware');
  if (!existsSync(folder)) return [];
  const ids = boards.map(board => board.id);
  return readdirSync(folder).filter(name => name.endsWith('.yaml')).sort().map(file => {
    const id = file.slice(0, -'.yaml'.length);
    const entry = parse(readFileSync(path.join(folder, file), 'utf8')) ?? {};
    if (!idPattern.test(id) || typeof entry.name !== 'string' || typeof entry.description !== 'string') throw Error(`Invalid firmware entry ${file}`);
    let selected = entry.boards === 'all' ? ids : entry.boards;
    if (!Array.isArray(selected) || selected.some(board => !ids.includes(board))) throw Error(`${file}: boards must be "all" or known board IDs`);
    if (entry.requires_frame_grabber === true) selected = selected.filter(board => boards.find(item => item.id === board).grabbers.length);
    const after = entry.after_flash ?? {};
    if (after.link !== undefined && !/^(\/(?!\/)|https:\/\/)/.test(after.link)) throw Error(`${file}: after_flash.link must be a site path such as /projects/webrtc, or start with https://`);
    let provisioning = null;
    if (entry.provisioning !== undefined) {
      const page = entry.provisioning?.page;
      const resolved = typeof page === 'string' ? path.resolve(workspace, page) : '';
      if (Object.keys(entry.provisioning ?? {}).join() !== 'page' || typeof page !== 'string' || !page.startsWith('projects/') || !page.endsWith('.html')
        || !resolved.startsWith(path.join(workspace, 'projects') + path.sep) || !existsSync(resolved)) {
        throw Error(`${file}: provisioning.page must be an existing .html file under projects/`);
      }
      provisioning = { page: resolved };
    }
    return { id, name: entry.name, description: entry.description, boards: [...new Set(selected)].sort(), afterFlash: { text: after.text ?? '', link: after.link ?? '' }, provisioning, file: path.join(folder, file) };
  });
}

// Packages per project and board; private ones contain private settings such as a secret.
export function readFirmware(workspace, firmwares, { includePrivate }) {
  const root = path.join(workspace, 'build/firmware');
  const manifest = {};
  if (!existsSync(root)) return manifest;
  for (const firmware of firmwares) {
    const project = firmware.id;
    if (!existsSync(path.join(root, project))) continue;
    for (const board of readdirSync(path.join(root, project)).filter(name => firmware.boards.includes(name)).sort()) {
      const folder = path.join(root, project, board);
      let info;
      try { info = JSON.parse(readFileSync(path.join(folder, 'build.json'), 'utf8')); } catch { continue; }
      if (info.board !== board || info.project !== project || (info.private && !includePrivate)) continue;
      (manifest[project] ??= {})[board] = { commit: info.commit, built: info.built, frame_grabber: info.frame_grabber, private: Boolean(info.private), image: info.image ?? 'firmware.bin', app_offset: info.app_offset ?? '0x10000' };
    }
  }
  return manifest;
}

// Files a package may serve: what its flasher_args.json lists, plus metadata.
export function packageFiles(folder) {
  const args = JSON.parse(readFileSync(path.join(folder, 'flasher_args.json'), 'utf8'));
  return ['flasher_args.json', 'firmware_selection.json', 'build.json', ...(existsSync(path.join(folder, 'firmware.bin')) ? ['firmware.bin'] : []), ...Object.values(args.flash_files)];
}

function catalogModule(boards, firmwares) {
  const imports = boards.map((board, index) => board.image ? `import image${index} from ${JSON.stringify(board.image)};` : '').join('\n');
  const items = boards.map((board, index) => `{ id: ${JSON.stringify(board.id)}, name: ${JSON.stringify(board.name)}, target: ${JSON.stringify(board.target)}, image: ${board.image ? `image${index}` : 'null'} }`);
  // Each project's provisioning page is its own chunk, loaded when the popup opens.
  const projects = firmwares.map(({ id, name, description, boards: ids, afterFlash, provisioning }) => {
    const fields = JSON.stringify({ id, name, description, boards: ids, afterFlash }).slice(0, -1);
    const page = provisioning ? `{ page: () => import(${JSON.stringify(pagePrefix + id)}).then(module => module.default) }` : 'null';
    return `${fields},"provisioning":${page}}`;
  });
  return `${imports}\nexport const boards = [${items.join(', ')}];\nexport const firmwares = [${projects.join(', ')}];\n`;
}

export function boardsPlugin(workspace) {
  const firmwareRoot = path.join(workspace, 'build/firmware');
  return {
    name: 'board-catalog',
    resolveId(id) {
      if (id === virtualId) return resolvedId;
      return id.startsWith(pagePrefix) ? '\0' + id : null;
    },
    load(id) {
      if (id.startsWith('\0' + pagePrefix)) {
        const project = id.slice(1 + pagePrefix.length);
        const page = readFirmwareCatalog(workspace).find(firmware => firmware.id === project)?.provisioning?.page;
        if (!page) throw Error(`No provisioning page for ${project}`);
        this.addWatchFile(page);
        return `export default ${JSON.stringify(readFileSync(page, 'utf8'))};`;
      }
      if (id !== resolvedId) return null;
      const boards = readCatalog(workspace);
      const firmwares = readFirmwareCatalog(workspace, boards);
      for (const file of new Set([...boards, ...firmwares].map(item => item.file))) this.addWatchFile(file);
      return catalogModule(boards, firmwares);
    },
    configureServer(server) {
      const catalogs = [path.join(workspace, 'config/boards'), path.join(workspace, 'config/firmware')];
      server.watcher.add(catalogs);
      server.watcher.on('all', (_, file) => {
        if (!catalogs.some(folder => file.startsWith(folder))) return;
        const module = server.moduleGraph.getModuleById(resolvedId);
        if (module) { server.moduleGraph.invalidateModule(module); server.ws.send({ type: 'full-reload' }); }
      });
      // The dev server serves every local package, private ones included, to this computer only.
      server.middlewares.use('/firmware/', (request, response, next) => {
        const parts = decodeURIComponent(request.url.split('?')[0]).replace(/^\/+/, '').split('/');
        const send = (status, body, type = 'application/json') => { response.statusCode = status; response.setHeader('Content-Type', type); response.setHeader('Cache-Control', 'no-store'); response.end(body); };
        if (parts.join('/') === 'index.json') return send(200, JSON.stringify(readFirmware(workspace, readFirmwareCatalog(workspace), { includePrivate: true })));
        const [project, board, ...rest] = parts;
        const folder = path.join(firmwareRoot, project ?? '', board ?? '');
        const file = rest.join('/');
        try {
          if (!idPattern.test(project) || !idPattern.test(board) || !packageFiles(folder).includes(file)) return send(404, '{"error":"Not found"}');
          const target = path.resolve(folder, file);
          if (!target.startsWith(folder + path.sep) || !statSync(target).isFile()) return send(404, '{"error":"Not found"}');
          return send(200, readFileSync(target), types[path.extname(file)] ?? 'application/octet-stream');
        } catch { return send(404, '{"error":"Not found"}'); }
      });
    },
    generateBundle() {
      // Published sites carry only packages without a signaling secret.
      const firmwares = readFirmwareCatalog(workspace);
      const all = readFirmware(workspace, firmwares, { includePrivate: true });
      const manifest = readFirmware(workspace, firmwares, { includePrivate: false });
      for (const [project, entries] of Object.entries(all)) {
        for (const board of Object.keys(entries)) {
          if (!manifest[project]?.[board]) { this.warn(`Skipping private firmware ${project}/${board}: it has private settings such as a signaling secret`); continue; }
          const folder = path.join(firmwareRoot, project, board);
          for (const file of packageFiles(folder)) this.emitFile({ type: 'asset', fileName: `firmware/${project}/${board}/${file}`, source: readFileSync(path.join(folder, file)) });
        }
      }
      this.emitFile({ type: 'asset', fileName: 'firmware/index.json', source: JSON.stringify(manifest) });
    },
  };
}
