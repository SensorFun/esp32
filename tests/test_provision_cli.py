"""Automated provisioning must never read the controlling terminal or echo input."""
import importlib.util
import io
from pathlib import Path
import sys

import pytest


@pytest.fixture
def provision(monkeypatch):
    spec = importlib.util.spec_from_file_location('provision_cli', Path(__file__).resolve().parents[1] / 'tools/provision.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    def no_prompt(*args, **kwargs):
        pytest.fail('Automated provisioning attempted an interactive prompt')

    monkeypatch.setattr(module.getpass, 'getpass', no_prompt)
    monkeypatch.setattr('builtins.input', no_prompt)
    monkeypatch.setattr(module.time, 'sleep', lambda _: None)
    return module


def test_stdin_credentials_do_not_prompt_or_echo(provision, monkeypatch, capsys):
    writes = []

    class Console:
        def __init__(self, *args, **kwargs):
            self.messages = iter((b'No saved Wi-Fi; waiting for USB provisioning', b'Local camera ready: https://192.0.2.1/'))

        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

        def read(self, _):
            return next(self.messages)

        def write(self, data):
            writes.append(data)

    monkeypatch.setattr(provision.serial, 'Serial', Console)
    monkeypatch.setattr(sys, 'argv', ['provision.py', '--port', 'synthetic-port', '--camera-password', '--credentials-stdin'])
    # Synthetic test vectors only; preserve spaces instead of stripping passwords.
    lines = ['Example Test Network', ' synthetic-wifi-vector ', 'synthetic-camera-vector']
    monkeypatch.setattr(sys, 'stdin', io.StringIO('\n'.join(lines) + '\n'))
    provision.main()
    assert writes == [('wifi-provision\n' + '\n'.join(lines) + '\n').encode()]
    output = capsys.readouterr()
    assert 'Provisioned;' in output.out
    assert output.err == ''
    assert all(value not in output.out for value in lines)
    assert 'password:' not in output.out.lower()


def test_truncated_stdin_fails_without_prompting(provision, monkeypatch, capsys):
    monkeypatch.setattr(sys, 'argv', ['provision.py', '--port', 'synthetic-port', '--camera-password', '--credentials-stdin'])
    monkeypatch.setattr(sys, 'stdin', io.StringIO('Example Test Network\nsynthetic-wifi-vector\n'))
    with pytest.raises(SystemExit) as error:
        provision.main()
    assert error.value.code == 2
    output = capsys.readouterr()
    assert 'Missing credential line on stdin' in output.err
    assert 'synthetic-wifi-vector' not in output.err
