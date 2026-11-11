"""Read board metadata from the same YAML profiles composed by Hydra."""
import json
from pathlib import Path
import re
import sys

from omegaconf import OmegaConf


def load_board(root: Path, board: str) -> dict:
    if not isinstance(board, str) or not re.fullmatch(r"[a-zA-Z0-9_]+", board):
        raise ValueError(f"Invalid board name: {board}")
    path = root / "config" / "board" / f"{board}.yaml"
    if not path.is_file():
        raise ValueError(f"Unknown board: {board}")
    config = OmegaConf.load(path)
    if config.board != board:
        raise ValueError(f"Board profile name does not match its filename: {board}")
    metadata = OmegaConf.to_container(config.hardware, resolve=True, throw_on_missing=True)
    if not isinstance(metadata, dict) or not re.fullmatch(r"esp32[a-z0-9]*", str(metadata.get("target", ""))):
        raise ValueError(f"Invalid chip target for {board}")
    sensors = metadata.get("camera_sensors", [])
    if not isinstance(sensors, list) or any(not isinstance(sensor, str) or not re.fullmatch(r"ov[0-9]+", sensor) for sensor in sensors):
        raise ValueError(f"Invalid camera sensors for {board}")
    if metadata.get("default_camera", "none") not in ["none", *sensors]:
        raise ValueError(f"Default camera is unsupported for {board}")
    return metadata


if __name__ == "__main__":
    # CMake consumes this transient output; no board JSON files are maintained.
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python -m esp32_workspace.boards WORKSPACE BOARD")
    print(json.dumps(load_board(Path(sys.argv[1]), sys.argv[2])))
