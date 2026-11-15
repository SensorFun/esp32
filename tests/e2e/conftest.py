"""Hardware fixtures for tests/e2e: tests/config/e2e.yaml lists the boards
under test; each board's USB port is auto-detected at fixture time (not
cached), since the same board can enumerate on a different port between runs.
"""
import getpass
import os
from pathlib import Path
import re
import sys

from hydra import compose, initialize_config_dir
import pytest

ROOT = Path(__file__).resolve().parents[2]
TESTS_CONFIG = ROOT / "tests" / "config"
TESTS_SECRETS = ROOT / "tests" / ".secrets"
sys.path.insert(0, str(ROOT / "python"))
from esp32_workspace.flashing import detect_port  # noqa: E402

_terminal = None


@pytest.hookimpl(trylast=True)
def pytest_configure(config):
    global _terminal
    _terminal = config.pluginmanager.getplugin('terminalreporter')


def pytest_runtest_logreport(report):
    # Workers have no terminal reporter. The controller shows failures as they
    # arrive, even while the other board is still running its long pipeline.
    if report.failed and _terminal is not None:
        _terminal.write_sep('=', f'Board failure: {report.nodeid}')
        _terminal.write_line(str(report.longrepr))
        for output in (report.capstdout, report.capstderr):
            if output:
                _terminal.write_line('\n'.join(output.splitlines()[-80:]))


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    xml_path = item.config.getoption('xmlpath')
    if report.failed and xml_path:
        # Write in the worker immediately; xdist can delay captured terminal
        # output until the other board finishes its entire pipeline.
        name = re.sub(r'[^a-zA-Z0-9_.-]', '_', report.nodeid)
        path = Path(xml_path).parent / f'{name}.{report.when}.failure.log'
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, 'w') as output:
            output.write(str(report.longrepr) + '\n' + report.capstdout + '\n' + report.capstderr)


class Secret(str):
    """A string that never reveals its value in a repr/traceback/locals dump.

    pytest prints fixture values verbatim (via repr) in failure reports, so a
    plain str/tuple holding a real credential leaks into the terminal and any
    captured log on the very first failure. __str__ still returns the real
    value, so formatting/encoding/subprocess use is unaffected.
    """
    __slots__ = ()

    def __repr__(self):
        return "<redacted>"


def _compose(config_dir, config_name, overrides=()):
    # Each call opens and tears down its own Hydra context, so composing the
    # per-device "firmware" config repeatedly here is safe and side-effect-free
    # (no chdir/logging; that only happens under a real @hydra.main entrypoint).
    with initialize_config_dir(config_dir=str(config_dir), version_base="1.3"):
        return compose(config_name=config_name, overrides=list(overrides))


def _board_name(device):
    return _compose(ROOT / "config", "firmware", [f"device={device}"]).board


def pytest_addoption(parser):
    parser.addoption('--board-device', action='append', help='Select a device from tests/config/e2e.yaml')


def pytest_generate_tests(metafunc):
    if "board" not in metafunc.fixturenames:
        return
    e2e = _compose(TESTS_CONFIG, "e2e")
    if not e2e.devices:
        raise pytest.UsageError("tests/config/e2e.yaml lists no devices; set devices to the boards under test")
    selected = metafunc.config.getoption('--board-device')
    devices = selected if selected is not None else list(e2e.devices)
    if len(set(devices)) != len(devices) or any(d not in e2e.devices for d in devices):
        raise pytest.UsageError('Select distinct devices listed in tests/config/e2e.yaml')
    params = [{"device": device, "board": _board_name(device), "server": e2e.server, "browser": e2e.browser}
              for device in devices]
    if len({p['board'] for p in params}) != len(params):
        raise pytest.UsageError('Devices must have distinct board build directories')
    metafunc.parametrize("board", params, ids=devices, indirect=True)


@pytest.fixture
def board(request):
    info = dict(request.param)
    if not info["browser"]:
        raise pytest.UsageError("tests/config/e2e.yaml: set browser to an installed Chromium executable")
    # Detected fresh for every test, not reused across the session: the same
    # board can enumerate on a different /dev/ttyUSB* after a replug or reset.
    info["port"] = detect_port(ROOT, info["board"])
    return info


def _read_secret(name):
    try:
        return (TESTS_SECRETS / name).read_text().rstrip("\r\n") or None
    except OSError:
        return None


@pytest.fixture(scope="session")
def wifi_credentials():
    # Never read from tests/config/e2e.yaml: that file is tracked. Precedence:
    # environment, then the ignored tests/.secrets files, then an interactive
    # prompt as a last resort.
    ssid = os.environ.get("E2E_WIFI_SSID") or _read_secret("wifi_ssid")
    password = os.environ.get("E2E_WIFI_PASSWORD") or _read_secret("wifi_password")
    if not (ssid and password):
        try:
            ssid = ssid or input("Wi-Fi SSID: ")
            password = password or getpass.getpass("Wi-Fi password: ")
        except OSError as error:
            raise pytest.UsageError(
                "Set E2E_WIFI_SSID/E2E_WIFI_PASSWORD, put them in tests/.secrets/wifi_ssid and "
                "tests/.secrets/wifi_password, or run with -s to enter them interactively"
            ) from error
    return Secret(ssid), Secret(password)
