"""Compose test and firmware configuration without Hydra output directories."""
from hydra import compose, initialize_config_dir


def compose_config(config_dir, config_name, overrides=()):
    with initialize_config_dir(config_dir=str(config_dir), version_base='1.3'):
        return compose(config_name=config_name, overrides=list(overrides))
