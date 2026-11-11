"""Thin wrapper around ESP-IDF's native CMake build frontend."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess


from .boards import load_board


def command(root: Path, project: str, board: str, action: str, port: str | None, camera: str | None = None) -> list[str]:
    projects = root / "projects"
    source = (projects / project).resolve()
    if source.parent != projects.resolve() or not (source / "CMakeLists.txt").is_file():
        raise ValueError(f"Unknown project: {project}")
    config = load_board(root, board)
    if camera is None:
        camera = config.get("default_camera", "none")
    if camera != "none" and camera not in config.get("camera_sensors", []):
        raise ValueError(f"Camera {camera!r} is not supported by board {board}")
    build = root / "build" / project / board
    defaults = [source / "sdkconfig.defaults"]
    defaults += [root / "boards" / item for item in config.get("sdkconfig_defaults", [])]
    defaults = [path for path in defaults if path.is_file()]
    result = [
        "idf.py", "-C", str(source), "-B", str(build),
        f"-DIDF_TARGET={config['target']}", f"-DBOARD={board}",
        f"-DCAMERA_SENSOR={camera}",
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
        print("Boards: " + ", ".join(p.stem for p in sorted((root / "config/board").glob("*.yaml"))))
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
