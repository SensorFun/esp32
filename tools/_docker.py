"""Standard-library bootstrap for running firmware tools inside Docker."""
import os
from pathlib import Path
import subprocess
import sys


def run_in_docker(script: str) -> None:
    if os.environ.get("ESP32_IN_DOCKER") == "1":
        return
    source = Path(script).resolve()
    root = source.parents[1]
    command = [
        str(root / "docker" / "docker.sh"), "--build", "--",
        "python", f"/workspace/tools/{source.name}", *sys.argv[1:],
    ]
    raise SystemExit(subprocess.call(command, cwd=root))
