"""Thin wrapper around ESP-IDF's native CMake build frontend."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess


from .boards import BOARD_DEFAULTS, load_board, load_catalog, write_board_sdkconfig


def command(root: Path, project: str, board: str, action: str, port: str | None, frame_grabber: str | None = None) -> list[str]:
    projects = root / "projects"
    source = (projects / project).resolve()
    if source.parent != projects.resolve() or not (source / "CMakeLists.txt").is_file():
        raise ValueError(f"Unknown project: {project}")
    config = load_board(root, board)
    if frame_grabber is not None and frame_grabber not in config.get("supported_frame_grabber", []):
        raise ValueError(f"Frame grabber {frame_grabber!r} is not supported by board {board}")
    build = root / "build" / project / board
    defaults = [source / "sdkconfig.defaults"]
    defaults += [root / "boards" / item for item in config.get("sdkconfig_defaults", [])]
    defaults = [path for path in defaults if path.is_file()]
    # Board values from config/boards, written by write_board_sdkconfig before idf.py runs.
    defaults.append(build / BOARD_DEFAULTS)
    result = [
        "idf.py", "-C", str(source), "-B", str(build),
        f"-DIDF_TARGET={config['target']}", f"-DBOARD={board}",
        f"-DFRAME_GRABBER={frame_grabber if frame_grabber is not None else 'none'}",
        f"-DSDKCONFIG={build / 'sdkconfig'}",
    ]
    result.append("-DSDKCONFIG_DEFAULTS=" + ";".join(map(str, defaults)))
    if port:
        result += ["-p", port]
    return result + [action]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["list", "build", "flash", "monitor", "menuconfig", "clean"])
    parser.add_argument("--root", type=Path, default=Path(os.environ.get("ESP32_WORKSPACE", Path.cwd())))
    parser.add_argument("--project", default="webrtc")
    parser.add_argument("--board", default="esp32_ai-thinker_cam")
    parser.add_argument("--frame-grabber", help="Frame grabber profile from config/frame_grabber")
    parser.add_argument("--port", help="Serial device, e.g. /dev/ttyACM0")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.action == "list":
        print("Projects: " + ", ".join(p.parent.name for p in sorted((root / "projects").glob("*/CMakeLists.txt"))))
        print("Boards: " + ", ".join(sorted(load_catalog(root))))
        return 0
    try:
        cmd = command(root, args.project, args.board, args.action, args.port, args.frame_grabber)
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    print(shlex.join(cmd), flush=True)
    if args.dry_run:
        return 0
    if not shutil.which("idf.py"):
        parser.error("idf.py is unavailable; use docker/docker.sh or source ESP-IDF's export.sh")
    write_board_sdkconfig(root, args.board, root / "build" / args.project / args.board)
    return subprocess.call(cmd)
