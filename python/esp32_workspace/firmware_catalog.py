"""Flashable firmware catalog: config/firmware/<project>.yaml.

Each file makes projects/<project> available to CI builds and the browser
flasher, for the boards it lists. Uses plain PyYAML so host tools and CI can
list builds without Hydra.
"""
from pathlib import Path
import re

import yaml

ID_RE = re.compile(r'[A-Za-z0-9_-]+')
SETTING_RE = re.compile(r'CONFIG_[A-Z0-9_]+')


def board_entries(root: Path) -> dict:
    boards = {}
    for path in sorted((root / 'config/boards').glob('*.yaml')):
        entries = yaml.safe_load(path.read_text()) or {}
        if not isinstance(entries, dict):
            raise ValueError(f'Invalid board catalog: {path.name}')
        for board, entry in entries.items():
            if not isinstance(board, str) or not ID_RE.fullmatch(board) or not isinstance(entry, dict):
                raise ValueError(f'Invalid board entry {board!r} in {path.name}')
            if board in boards:
                raise ValueError(f'Duplicate board: {board}')
            boards[board] = entry
    return boards


def load_firmware(root: Path, check_sources: bool = True) -> dict:
    """check_sources=False skips projects/<project> checks where only one project is mounted."""
    boards = board_entries(root)
    catalog = {}
    for path in sorted((root / 'config/firmware').glob('*.yaml')):
        project = path.stem
        entry = yaml.safe_load(path.read_text())
        if not ID_RE.fullmatch(project) or (check_sources and not (root / 'projects' / project / 'CMakeLists.txt').is_file()):
            raise ValueError(f'{path.name}: no firmware project projects/{project}')
        if not isinstance(entry, dict):
            raise ValueError(f'{path.name}: expected a mapping')
        unknown = set(entry) - {'name', 'description', 'boards', 'requires_frame_grabber', 'private_settings', 'after_flash', 'provisioning'}
        if unknown:
            raise ValueError(f'{path.name}: unknown fields {sorted(unknown)}')
        for field in ('name', 'description'):
            if not isinstance(entry.get(field), str) or not entry[field].strip():
                raise ValueError(f'{path.name}: {field} is required')
        listed = entry.get('boards')
        if listed == 'all':
            selected = list(boards)
        elif isinstance(listed, list) and listed and all(isinstance(board, str) for board in listed):
            missing = sorted(set(listed) - set(boards))
            if missing:
                raise ValueError(f'{path.name}: unknown boards {missing}')
            selected = list(dict.fromkeys(listed))
        else:
            raise ValueError(f'{path.name}: boards must be "all" or a list of board IDs')
        requires = entry.get('requires_frame_grabber', False)
        if not isinstance(requires, bool):
            raise ValueError(f'{path.name}: requires_frame_grabber must be true or false')
        if requires:
            selected = [board for board in selected if boards[board].get('supported_frame_grabber')]
        private = entry.get('private_settings', [])
        if not isinstance(private, list) or any(not isinstance(name, str) or not SETTING_RE.fullmatch(name) for name in private):
            raise ValueError(f'{path.name}: private_settings must be a list of CONFIG_ names')
        after = entry.get('after_flash', {})
        if not isinstance(after, dict) or set(after) - {'text', 'link'} or any(not isinstance(value, str) for value in after.values()):
            raise ValueError(f'{path.name}: after_flash takes text and link strings')
        if 'link' in after and not re.match(r'(#/|https://)', after['link']):
            raise ValueError(f'{path.name}: after_flash.link must start with #/ or https://')
        provisioning = entry.get('provisioning')
        if provisioning is not None:
            page = provisioning.get('page') if isinstance(provisioning, dict) else None
            if (not isinstance(page, str) or set(provisioning) != {'page'} or not page.startswith('projects/')
                    or not page.endswith('.html') or '..' in Path(page).parts or '\\' in page):
                raise ValueError(f'{path.name}: provisioning takes a page path under projects/ ending in .html')
            if check_sources and not (root / page).is_file():
                raise ValueError(f'{path.name}: provisioning page {page} does not exist')
        catalog[project] = {'name': entry['name'], 'description': entry['description'], 'boards': sorted(selected),
                            'private_settings': private, 'after_flash': after, 'provisioning': provisioning}
    return catalog


def build_pairs(root: Path, projects=None, boards=None, check_sources: bool = True) -> list[dict]:
    """Project and board pairs to build, optionally filtered."""
    catalog = load_firmware(root, check_sources)
    for name, known in (('project', set(catalog)), ('board', {b for entry in catalog.values() for b in entry['boards']})):
        wanted = projects if name == 'project' else boards
        unknown = sorted(set(wanted or []) - known)
        if unknown:
            raise ValueError(f'Unknown or unflashable {name}: ' + ', '.join(unknown))
    return [{'project': project, 'board': board}
            for project, entry in catalog.items() if not projects or project in projects
            for board in entry['boards'] if not boards or board in boards]


def is_private(sdkconfig_text: str, settings: list[str]) -> bool:
    """True when any listed setting has a non-empty value."""
    for name in settings:
        match = re.search(rf'^{name}=(.*)$', sdkconfig_text, re.MULTILINE)
        if match and match.group(1).strip() not in ('', '""', 'n'):
            return True
    return False
