"""Load board catalogs for host tools and CMake."""
import json
from pathlib import Path
import re
import sys

from omegaconf import OmegaConf


def load_catalog(root: Path) -> dict:
    catalog = {}
    for path in sorted((root / 'config/boards').glob('*.yaml')):
        entries = OmegaConf.to_container(OmegaConf.load(path), resolve=True, throw_on_missing=True)
        if not isinstance(entries, dict):
            raise ValueError(f'Invalid board catalog: {path.name}')
        for board, metadata in entries.items():
            if not isinstance(board, str) or not re.fullmatch(r'[a-zA-Z0-9_-]+', board):
                raise ValueError(f'Invalid board name: {board}')
            if board in catalog:
                raise ValueError(f'Duplicate board: {board}')
            if not isinstance(metadata, dict) or not re.fullmatch(r'esp32[a-z0-9]*', str(metadata.get('target', ''))):
                raise ValueError(f'Invalid chip target for {board}')
            if not isinstance(metadata.get('name'), str) or not metadata['name'].strip():
                raise ValueError(f'Missing display name for {board}')
            header = metadata.get('header')
            if not isinstance(header, str) or not header or Path(header).is_absolute():
                raise ValueError(f'Invalid header for {board}')
            header_path = (root / 'boards' / header).resolve()
            if not header_path.is_relative_to((root / 'boards').resolve()) or not header_path.is_file():
                raise ValueError(f'Missing or invalid header for {board}: {header}')
            image = metadata.get('image')
            if image is not None:
                images = (root / 'config/boards/images').resolve()
                image_path = (root / 'config/boards' / str(image)).resolve()
                if not isinstance(image, str) or Path(image).is_absolute() or not image_path.is_relative_to(images) or not image_path.is_file():
                    raise ValueError(f'Missing or invalid image for {board}: {image}')
            grabbers = metadata.get('supported_frame_grabber', [])
            if not isinstance(grabbers, list) or any(
                not isinstance(name, str) or not re.fullmatch(r'[a-zA-Z0-9_]+', name)
                or not (root / 'config/frame_grabber' / f'{name}.yaml').is_file()
                for name in grabbers
            ):
                raise ValueError(f'Invalid frame grabber profiles for {board}')
            defaults = metadata.get('sdkconfig_defaults', [])
            if not isinstance(defaults, list) or any(not isinstance(item, str) or Path(item).is_absolute() or not (root / 'boards' / item).resolve().is_relative_to((root / 'boards').resolve()) or not (root / 'boards' / item).is_file() for item in defaults):
                raise ValueError(f'Invalid SDK defaults for {board}')
            catalog[board] = metadata
    return catalog


def load_board(root: Path, board: str) -> dict:
    if not isinstance(board, str) or not re.fullmatch(r'[a-zA-Z0-9_-]+', board):
        raise ValueError(f'Invalid board name: {board}')
    catalog = load_catalog(root)
    if board not in catalog:
        raise ValueError(f'Unknown board: {board}')
    return catalog[board]


def write_board_header(root: Path, board: str, output: Path) -> None:
    metadata = load_board(root, board)
    # JSON string quoting safely preserves quotes and backslashes in C++ names.
    name = json.dumps(metadata['name'], ensure_ascii=False)
    header = (root / 'boards' / metadata['header']).resolve().as_posix()
    content = f'#pragma once\ninline constexpr char board_display_name[] = {name};\n#include {json.dumps(header)}\n'
    if not output.exists() or output.read_text() != content:
        output.write_text(content)


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        raise SystemExit('Usage: python -m esp32_workspace.boards WORKSPACE BOARD [OUTPUT_HEADER]')
    root, board = Path(sys.argv[1]), sys.argv[2]
    metadata = load_board(root, board)
    if len(sys.argv) == 4:
        write_board_header(root, board, Path(sys.argv[3]))
    print(json.dumps(metadata))
