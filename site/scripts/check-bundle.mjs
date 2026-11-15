// Checks firmware bundled into a built site: complete, flashable, and never private.
// Usage: node scripts/check-bundle.mjs dist [--all-boards]
// --all-boards requires a package for every project/board pair in config/firmware.
import { existsSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readCatalog, readFirmwareCatalog } from './boards-plugin.mjs';
import { checkLayout, chipId, parseFlasherArgs } from '../src/projects/flasher/plan.mjs';

export function checkBundle(dist, workspace, { allBoards = false } = {}) {
  const catalog = readCatalog(workspace);
  const firmwares = readFirmwareCatalog(workspace, catalog);
  const manifest = JSON.parse(readFileSync(path.join(dist, 'firmware/index.json'), 'utf8'));
  const problems = [];
  const bundled = [];
  for (const project of Object.keys(manifest)) if (!firmwares.some(item => item.id === project)) problems.push(`${project}: not in config/firmware`);
  for (const firmware of firmwares) {
    const project = firmware.id;
    const packages = manifest[project] ?? {};
    if (allBoards) for (const board of firmware.boards) if (!packages[board]) problems.push(`${project}/${board}: no firmware package`);
    for (const [id, entry] of Object.entries(packages)) {
      const board = catalog.find(item => item.id === id);
      const folder = path.join(dist, 'firmware', project, id);
      try {
        if (!board || !firmware.boards.includes(id)) throw Error(`config/firmware/${project}.yaml does not offer this board`);
        if (entry.private) throw Error('private build (has private settings such as a signaling secret) must not be published');
        const info = JSON.parse(readFileSync(path.join(folder, 'build.json'), 'utf8'));
        if (info.private) throw Error('build.json marks this build private');
        if (info.project !== project || info.board !== id) throw Error(`build.json is for ${info.project}/${info.board}`);
        const plan = parseFlasherArgs(readFileSync(path.join(folder, 'flasher_args.json')));
        if (plan.chip !== chipId(board.target)) throw Error(`chip ${plan.chip} does not match target ${board.target}`);
        const selection = JSON.parse(readFileSync(path.join(folder, 'firmware_selection.json'), 'utf8'));
        if (selection.board !== id) throw Error(`firmware_selection.json names ${selection.board}`);
        const sizes = plan.files.map(file => {
          const target = path.join(folder, file.path);
          if (!existsSync(target)) throw Error(`missing ${file.path}`);
          return readFileSync(target).length;
        });
        if (!existsSync(path.join(folder, 'firmware.bin'))) throw Error('missing firmware.bin download');
        checkLayout(plan, sizes);
        bundled.push(`${project}/${id}`);
      } catch (error) { problems.push(`${project}/${id}: ${error.message}`); }
    }
  }
  return { bundled, problems };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const dist = path.resolve(process.argv[2] ?? 'dist');
  const workspace = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
  const { bundled, problems } = checkBundle(dist, workspace, { allBoards: process.argv.includes('--all-boards') });
  if (problems.length) { console.error('Firmware bundle problems:\n  ' + problems.join('\n  ')); process.exit(1); }
  console.log(`Bundled firmware OK: ${bundled.join(', ') || 'none'}`);
}
