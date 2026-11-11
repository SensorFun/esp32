"""Thin wrapper around ESP-IDF's native CMake build frontend."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess


def command(root: Path, project: str, board: str, action: str, port: str | None) -> list[str]:
    projects = root / "projects"
    source = (projects / project).resolve()
    if source.parent != projects.resolve() or not (source / "CMakeLists.txt").is_file():
        raise ValueError(f"Unknown project: {project}")
    board_file = root / "boards" / f"{board}.json"
    if Path(board).name != board or not board_file.is_file():
        raise ValueError(f"Unknown board: {board}")
    config = json.loads(board_file.read_text())
    build = root / "build" / project / board
    defaults = [source / "sdkconfig.defaults"]
    defaults += [root / "boards" / item for item in config.get("sdkconfig_defaults", [])]
    defaults = [path for path in defaults if path.is_file()]
    result = [
        "idf.py", "-C", str(source), "-B", str(build),
        f"-DIDF_TARGET={config['target']}", f"-DBOARD={board}",
        f"-DSDKCONFIG={build / 'sdkconfig'}",
    ]
    if defaults:
        result.append("-DSDKCONFIG_DEFAULTS=" + ";".join(map(str, defaults)))
    if port:
        result += ["-p", port]
    return result + [action]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["list", "build", "flash", "monitor", "menuconfig", "clean"])
    parser.add_argument("--root", type=Path, default=Path(os.environ.get("ESP32_WORKSPACE", Path.cwd())))
    parser.add_argument("--project", default="hello_world")
    parser.add_argument("--board", default="esp32s3_seeed_cam")
    parser.add_argument("--port", help="Serial device, e.g. /dev/ttyACM0")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.action == "list":
        print("Projects: " + ", ".join(p.parent.name for p in sorted((root / "projects").glob("*/CMakeLists.txt"))))
        print("Boards: " + ", ".join(p.stem for p in sorted((root / "boards").glob("*.json"))))
        return 0
    try:
        cmd = command(root, args.project, args.board, args.action, args.port)
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    print(shlex.join(cmd), flush=True)
    if args.dry_run:
        return 0
    if not shutil.which("idf.py"):
        parser.error("idf.py is unavailable; use docker/docker.sh or source ESP-IDF's export.sh")
    return subprocess.call(cmd)
