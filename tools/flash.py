#!/usr/bin/env python3
"""Flash built firmware locally with esptool and automatic serial detection."""
from pathlib import Path

import hydra
from omegaconf import DictConfig

from esp32_workspace.flashing import run_flash


@hydra.main(version_base="1.3", config_path="../config", config_name="flash")
def main(config: DictConfig) -> None:
    raise SystemExit(run_flash(Path(__file__).resolve().parents[1], config))


if __name__ == "__main__":
    main()
