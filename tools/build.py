#!/usr/bin/env python3
"""Build ESP-IDF firmware selected by config/build.yaml and Hydra overrides."""
from pathlib import Path

from _docker import run_in_docker

# Dispatch before importing Hydra: the host only needs Python and Docker.
if __name__ == "__main__":
    run_in_docker(__file__)

import hydra
from omegaconf import DictConfig

from esp32_workspace.firmware import run_firmware


@hydra.main(version_base="1.3", config_path="../config", config_name="build")
def main(config: DictConfig) -> None:
    raise SystemExit(run_firmware(Path(__file__).resolve().parents[1], config, "build"))


if __name__ == "__main__":
    main()
