#!/usr/bin/env python3
#!/usr/bin/env python3
"""Build flashable firmware and package it for the browser flasher.

config/firmware/<project>.yaml lists which projects are flashable and for
which boards. CI and local runs use the same steps and espressif/idf image:

    python3 tools/firmware_artifacts.py                  # every project and board, in Docker
    python3 tools/firmware_artifacts.py --project webrtc --board ID
    python3 tools/firmware_artifacts.py --package-only   # package existing builds
    python3 tools/firmware_artifacts.py --ci             # fresh default builds, as in CI
    python3 tools/firmware_artifacts.py --list           # project/board pairs (CI matrix)

Packages land in build/firmware/<project>/<board>/, which the site serves at
firmware/<project>/<board>/. On the host each pair builds in its own
temporary espressif/idf container with read-only source mounts; inside a
container (ESP32_IN_DOCKER=1, as in CI) it builds directly.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
IDF_IMAGE = 'espressif/idf:v6.1'
HYDRA = 'hydra-core==1.3.2'
SOURCES = ('tools', 'python', 'config', 'boards', 'cmake', 'projects/common')
sys.path.insert(0, str(ROOT / 'python'))
from esp32_workspace.firmware_catalog import build_pairs, is_private, load_firmware  # noqa: E402

# A container mounts only the project it builds.
IN_CONTAINER = os.environ.get('ESP32_IN_DOCKER') == '1'


def commit_id(root: Path) -> str:
    try:
        head = subprocess.run(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=root, capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'
    return head + ('-dirty' if dirty else '')


def package(root: Path, project: str, board: str, commit: str) -> Path:
    """Copy one ESP-IDF build's flashable files into build/firmware."""
    from esp32_workspace.boards import load_board

    metadata = load_board(root, board)
    firmware = load_firmware(root, check_sources=not IN_CONTAINER).get(project)
    if not firmware or board not in firmware['boards']:
        raise ValueError(f'config/firmware does not offer {project} for {board}')
    build = (root / 'build' / project / board).resolve()
    args_file = build / 'flasher_args.json'
    if not args_file.is_file():
        raise ValueError(f'{project} is not built for {board}; build it first')
    flasher_args = json.loads(args_file.read_text())
    if flasher_args['extra_esptool_args']['chip'] != metadata['target']:
        raise ValueError(f'Build chip does not match {board}; rebuild it')
    selection = json.loads((build / 'firmware_selection.json').read_text())
    if selection.get('board') != board:
        raise ValueError(f'firmware_selection.json does not belong to {board}; rebuild it')
    # A build with any private setting (such as a signaling secret) is served by
    # the dev server only; the site build never publishes it.
    sdkconfig = build / 'sdkconfig'
    private = sdkconfig.is_file() and is_private(sdkconfig.read_text(), firmware['private_settings'])

    out = root / 'build' / 'firmware' / project / board
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    names = ['flasher_args.json', 'firmware_selection.json', *flasher_args['flash_files'].values()]
    for name in names:
        source = (build / name).resolve()
        if not source.is_relative_to(build) or not source.is_file():
            raise ValueError(f'Missing or invalid firmware file for {board}: {name}')
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    # A complete, single-file image for downloads and browser uploads. Preserve
    # the original flash offsets; gaps contain erased flash bytes.
    segments = sorted((int(offset, 0), (out / name).read_bytes())
                      for offset, name in flasher_args['flash_files'].items())
    end = 0
    for offset, data in segments:
        if offset < end or not data or offset + len(data) > 128 * 1024 * 1024:
            raise ValueError('Overlapping, empty, or oversized firmware segments')
        end = offset + len(data)
    merged = bytearray(b'\xff') * end
    for offset, data in segments:
        merged[offset:offset + len(data)] = data
    (out / 'firmware.bin').write_bytes(merged)
    info = {'project': project, 'board': board, 'target': metadata['target'],
            'frame_grabber': selection.get('frame_grabber'), 'commit': commit,
            'built': int(args_file.stat().st_mtime), 'private': private,
            'image': 'firmware.bin', 'app_offset': flasher_args.get('app', {}).get('offset', '0x10000')}
    (out / 'build.json').write_text(json.dumps(info, indent=2) + '\n')
    size = sum((out / name).stat().st_size for name in flasher_args['flash_files'].values())
    print(f'Packaged {project} for {board}: {size / 1024:.0f} KiB' + (' (private: has private settings)' if private else ''), flush=True)
    return out


def build_here(root: Path, project: str, board: str) -> None:
    """Build inside an ESP-IDF environment (CI job or our container)."""
    from esp32_workspace.boards import load_board

    grabbers = load_board(root, board).get('supported_frame_grabber', [])
    # Firmware detects the sensor at runtime; the grabber only labels the build.
    overrides = [f'project={project}', f'board={board}']
    if grabbers:
        overrides.append('frame_grabber=' + ('ov' if 'ov' in grabbers else grabbers[0]))
    env = {**os.environ, 'ESP32_IN_DOCKER': '1', 'PYTHONPATH': str(root / 'python')}
    result = subprocess.call([sys.executable, str(root / 'tools/build.py'), *overrides], cwd=root, env=env)
    if result:
        raise SystemExit(result)


def build_in_docker(root: Path, project: str, board: str, commit: str, ci: bool = False) -> None:
    """Mirror one CI matrix job: a fresh espressif/idf container per board.

    With ci=True the board builds in build/ci/ from a clean folder and default
    settings, so no local sdkconfig (or signaling secret) is used, and the
    package is then copied into build/firmware/ like a downloaded CI artifact.
    """
    build_root = root / 'build' / 'ci' if ci else root / 'build'
    if ci and (build_root / project / board).exists():
        shutil.rmtree(build_root / project / board)
    cache = root / 'build' / 'firmware-cache' / project / board
    components = cache / 'managed_components'
    components.mkdir(parents=True, exist_ok=True)
    # Mount points inside the read-only project must exist; managed_components is git-ignored.
    (root / 'projects' / project / 'managed_components').mkdir(exist_ok=True)
    writable = [(build_root, 'build'), (components, f'projects/{project}/managed_components')]
    source_lock = root / 'projects' / project / 'dependencies.lock'
    if source_lock.is_file():
        # Each build starts from the committed lock, like a fresh CI checkout.
        lock = cache / 'dependencies.lock'
        shutil.copyfile(source_lock, lock)
        writable.append((lock, f'projects/{project}/dependencies.lock'))
    build_root.mkdir(parents=True, exist_ok=True)
    command = ['docker', 'run', '--rm']
    for name in (*SOURCES, f'projects/{project}'):
        command += ['--mount', f'type=bind,source={root / name},target=/workspace/{name},readonly']
    for source, target in writable:
        command += ['--mount', f'type=bind,source={source},target=/workspace/{target}']
    command += ['-e', 'ESP32_IN_DOCKER=1', '-w', '/workspace', IDF_IMAGE, 'bash', '-c',
                f'python -m pip install --quiet {HYDRA} && exec python tools/firmware_artifacts.py --project "$1" --board "$2" --commit "$3"',
                'bash', project, board, commit]
    print(f'Building {project} for {board} in {IDF_IMAGE}…', flush=True)
    result = subprocess.call(command, cwd=root)
    if result:
        raise SystemExit(result)
    if ci:
        out = root / 'build' / 'firmware' / project / board
        if out.exists():
            shutil.rmtree(out)
        shutil.copytree(build_root / 'firmware' / project / board, out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--project', action='append', help='Project from config/firmware; repeat for several (default: all)')
    parser.add_argument('--board', action='append', help='Board ID from config/boards; repeat for several (default: all)')
    parser.add_argument('--package-only', action='store_true', help='Package existing build/<project>/<board> folders without building')
    parser.add_argument('--ci', action='store_true', help='Build each board fresh with default settings, exactly as CI does')
    parser.add_argument('--list', action='store_true', help='Print project/board pairs as JSON (CI matrix)')
    parser.add_argument('--commit', help=argparse.SUPPRESS)
    args = parser.parse_args()
    pairs = build_pairs(ROOT, args.project, args.board, check_sources=not IN_CONTAINER)
    if args.list:
        print(json.dumps(pairs))
        return 0
    if not pairs:
        parser.error('No flashable firmware matches; see config/firmware')
    commit = args.commit or commit_id(ROOT)
    in_container = IN_CONTAINER
    if args.ci and (in_container or args.package_only):
        parser.error('--ci runs on the host and builds in Docker')
    for pair in pairs:
        project, board = pair['project'], pair['board']
        if args.package_only:
            package(ROOT, project, board, commit)
        elif in_container:
            build_here(ROOT, project, board)
            package(ROOT, project, board, commit)
        else:
            build_in_docker(ROOT, project, board, commit, ci=args.ci)
    if not in_container:
        print(f'Firmware packages: {ROOT / "build/firmware"}', flush=True)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, OSError, json.JSONDecodeError) as error:
        print(f'firmware_artifacts: {error}', file=sys.stderr)
        raise SystemExit(1) from None
