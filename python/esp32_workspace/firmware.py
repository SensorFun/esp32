"""Execute configured builds through the native ESP-IDF frontend."""
from pathlib import Path
import shlex
import shutil
import subprocess

from omegaconf import DictConfig, OmegaConf

from .cli import command


def run_firmware(root: Path, config: DictConfig, action: str) -> int:
    if action != "build":
        raise ValueError(f"Unsupported firmware action: {action}")
    OmegaConf.to_container(config, resolve=True, throw_on_missing=True)
    for key in ("project", "board"):
        if not isinstance(config[key], str) or not config[key]:
            raise ValueError(f"{key} must be a nonempty string")
    if not isinstance(config.dry_run, bool):
        raise ValueError("dry_run must be true or false")
    cmd = command(root.resolve(), config.project, config.board, action, None)
    print(shlex.join(cmd), flush=True)
    if config.dry_run:
        return 0
    if not shutil.which("idf.py"):
        raise RuntimeError("idf.py is unavailable; use docker/docker.sh or source ESP-IDF's export.sh")
    return subprocess.call(cmd)
