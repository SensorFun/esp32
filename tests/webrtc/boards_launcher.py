"""Launch parallel hardware tests, or an isolated firmware build for one worker."""
import argparse
import codecs
import fcntl
import getpass
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from conftest_helpers import compose_config

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'python'))
from esp32_workspace.flashing import detect_port  # noqa: E402


def firmware_build(device):
    config = compose_config(ROOT / 'config', 'firmware', [f'device={device}'])
    board = config.board
    folder = ROOT / 'build' / 'webrtc' / board
    if not (folder / 'sdkconfig').is_file():
        raise RuntimeError(f'Configure signaling in build/webrtc/{board}/sdkconfig before hardware tests')
    components = folder / 'managed_components'
    components.mkdir(parents=True, exist_ok=True)
    lock = folder / 'dependencies.lock'
    if not lock.exists():
        shutil.copyfile(ROOT / 'projects/webrtc/dependencies.lock', lock)
    command = ['docker', 'run', '--rm']
    for name in ('tools/build.py', 'tools/_docker.py', 'python', 'config', 'boards', 'cmake', 'projects/common', 'projects/webrtc'):
        command += ['--mount', f'type=bind,source={ROOT / name},target=/workspace/{name},readonly']
    for source, target in ((folder, f'build/webrtc/{board}'),
                           (components, 'projects/webrtc/managed_components'),
                           (lock, 'projects/webrtc/dependencies.lock')):
        command += ['--mount', f'type=bind,source={source},target=/workspace/{target}']
    command += ['-e', 'ESP32_IN_DOCKER=1', '-e', 'PYTHONPATH=/workspace/python',
                'espressif/idf:v6.1', 'bash', '-c',
                'python -m pip install --quiet hydra-core && exec python /workspace/tools/build.py project=webrtc "device=$1"',
                'bash', device]
    return subprocess.call(command, cwd=ROOT)


def credentials():
    for name, filename, prompt, hidden in (
        ('E2E_WIFI_SSID', 'wifi_ssid', 'Wi-Fi SSID: ', False),
        ('E2E_WIFI_PASSWORD', 'wifi_password', 'Wi-Fi password: ', True),
    ):
        value = os.environ.get(name)
        if value is None:
            path = ROOT / 'tests/.secrets' / filename
            if path.is_file():
                value = path.read_text().rstrip('\r\n')
            elif sys.stdin.isatty():
                value = getpass.getpass(prompt) if hidden else input(prompt)
            else:
                raise RuntimeError(f'Set {name} or create ignored tests/.secrets/{filename}')
        if not value or any(c in value for c in '\r\n\0'):
            raise RuntimeError(f'{name} must be nonempty and contain no newlines or NUL bytes')
        if name == 'E2E_WIFI_SSID' and len(value.encode()) > 32:
            raise RuntimeError('Wi-Fi SSID exceeds 32 UTF-8 bytes')
        if name == 'E2E_WIFI_PASSWORD' and len(value.encode()) > 64:
            raise RuntimeError('Wi-Fi password exceeds 64 UTF-8 bytes')
        os.environ[name] = value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', action='append', help='Limit to a configured device; repeat to select several')
    parser.add_argument('--port', action='append', default=[], metavar='DEVICE=PATH',
                        help='Override USB detection for a selected device; repeat for multiple devices')
    parser.add_argument('--collect-only', action='store_true', help='List tests without accessing hardware or credentials')
    parser.add_argument('--build', metavar='DEVICE', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.build:
        return firmware_build(args.build)
    config = compose_config(ROOT / 'tests/webrtc/config', 'e2e')
    devices = args.device if args.device is not None else list(config.devices)
    if not devices or len(set(devices)) != len(devices) or any(d not in config.devices for d in devices):
        parser.error('Choose distinct devices listed in tests/webrtc/config/e2e.yaml')
    overrides = {}
    for override in args.port:
        device, separator, port = override.partition('=')
        if not separator or device not in devices or not port or device in overrides:
            parser.error('--port must be DEVICE=PATH for a selected device, once per device')
        overrides[device] = port
    boards = [compose_config(ROOT / 'config', 'firmware', [f'device={d}']).board for d in devices]
    if len(set(boards)) != len(boards):
        parser.error('Parallel devices must have distinct board build directories')
    command = [sys.executable, '-m', 'pytest', 'tests/webrtc/e2e', '-v', '--tb=short',
               '-n', str(len(devices)), '--dist', 'loadgroup']
    for device in devices:
        command += ['--board-device', device]
    if args.collect_only:
        # xdist does not distribute collect-only runs.
        command += ['-n', '0', '--collect-only']
        return subprocess.call(command, cwd=ROOT)
    for executable in ('docker', 'npm', 'node'):
        if not shutil.which(executable):
            raise RuntimeError(f'{executable} is required')
    subprocess.run(['docker', 'info', '--format', '{{.ServerVersion}}'], check=True, stdout=subprocess.DEVNULL)
    if not Path(config.browser).is_file():
        raise RuntimeError('Set browser in tests/webrtc/config/e2e.yaml to an installed Chromium executable')
    locks = []
    try:
        ports = []
        for device, board in zip(devices, boards):
            folder = ROOT / 'build/webrtc' / board
            if not (folder / 'sdkconfig').is_file():
                raise RuntimeError(f'Missing build/webrtc/{board}/sdkconfig')
            lock = (folder / '.boards-test.lock').open('a')
            locks.append(lock)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError(f'Another board test is using {board}') from None
            try:
                port = overrides[device] if device in overrides else detect_port(ROOT, board)
            except ValueError as error:
                raise RuntimeError(f'{device}: {error} Use --port {device}=/dev/<port>.') from None
            path = Path(port).resolve()
            if not path.is_char_device():
                raise RuntimeError(f'{device}: {port} is not a serial device')
            ports.append(str(path))
            command += ['--board-port', f'{device}={path}']
        if len(set(ports)) != len(ports):
            raise RuntimeError('Multiple devices resolve to the same USB port')
        credentials()
        print(f'Running {len(devices)} board pipelines in parallel: {", ".join(devices)}', flush=True)
        print('Each pipeline builds, flashes, provisions, and tests its own board.', flush=True)
        reports = ROOT / 'build/board-tests'
        reports.mkdir(parents=True, exist_ok=True)
        report = Path(tempfile.mkdtemp(prefix='run-', dir=reports))
        print(f'Full test report: {report / "run.log"}', flush=True)
        command += ['--junitxml', str(report / 'results.xml'), '-o', 'junit_logging=all']
        with (report / 'run.log').open('x') as log:
            (report / 'run.log').chmod(0o600)
            with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT) as process:
                decoder = codecs.getincrementaldecoder('utf-8')('replace')
                for chunk in iter(lambda: process.stdout.read1(8192), b''):
                    text = decoder.decode(chunk)
                    log.write(text)
                    log.flush()
                    print(text, end='', flush=True)
                text = decoder.decode(b'', final=True)
                log.write(text)
                print(text, end='', flush=True)
                return process.wait()
    finally:
        for lock in locks:
            lock.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'Board test launcher: {error}', file=sys.stderr)
        raise SystemExit(1) from None
    except KeyboardInterrupt:
        print('Board tests interrupted.', file=sys.stderr)
        raise SystemExit(130) from None
