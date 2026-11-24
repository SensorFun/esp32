"""Generate the motion detection and recording defaults from the workspace YAML.

config/motion/motion.yaml gives each setting its default and allowed values.
The firmware's settings struct and field table read the generated constants.
Missing or unknown settings, malformed value sets and defaults outside their
allowed values fail the build rather than being dropped.
"""
from pathlib import Path
import math
import sys
import yaml

# Setting -> (group, C++ type). Matches MotionSettings in motion_settings.h.
SETTINGS = {
    'enabled': ('detection', 'bool'), 'min_speed': ('detection', 'float'),
    'min_area': ('detection', 'float'), 'cell': ('detection', 'int'),
    'min_texture': ('detection', 'float'), 'blur': ('detection', 'int'),
    'trigger_seconds': ('detection', 'float'), 'overlay': ('detection', 'bool'),
    'record': ('recording', 'bool'), 'post_seconds': ('recording', 'int'),
    'max_clip_seconds': ('recording', 'int'), 'overwrite_oldest': ('recording', 'bool'),
}


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def on_grid(value, low, step):
    steps = (value - low) / step
    return abs(steps - round(steps)) < 1e-6


def setting(name, spec, kind):
    if not isinstance(spec, dict) or 'default' not in spec:
        raise ValueError(f'{name}: needs a default')
    kinds = [key for key in ('switch', 'options', 'range') if key in spec]
    if len(kinds) != 1 or set(spec) - {'default', kinds[0]}:
        raise ValueError(f'{name}: give exactly one of switch, options or range')
    shape, default = kinds[0], spec['default']
    if (shape == 'switch') != (kind == 'bool'):
        raise ValueError(f'{name}: {"must" if kind == "bool" else "cannot"} be a switch')
    if shape == 'switch':
        if spec['switch'] is not True or type(default) is not bool:
            raise ValueError(f'{name}: switch must be true and its default true or false')
        return {'default': 'true' if default else 'false'}
    if shape == 'options':
        options = spec['options']
        if (not isinstance(options, list) or len(options) < 2 or any(type(v) is not int for v in options)
                or options != sorted(set(options))):
            raise ValueError(f'{name}: options must be two or more unique ascending integers')
        if default not in options:
            raise ValueError(f'{name}: default {default} is not one of {options}')
        return {'default': str(default), 'options': options}
    bounds = spec['range']
    if not (isinstance(bounds, list) and len(bounds) == 3 and all(map(number, bounds))
            and bounds[0] < bounds[1] and bounds[2] > 0):
        raise ValueError(f'{name}: range must be [min, max, step] with min < max and step > 0')
    low, high, step = bounds
    if kind == 'int' and not all(type(v) is int for v in (low, high, step, default)):
        raise ValueError(f'{name}: range and default must be integers')
    if not number(default) or not low <= default <= high or not on_grid(default, low, step):
        raise ValueError(f'{name}: default {default} is not on the range {bounds}')
    literal = (lambda v: f'{float(v)!r}f') if kind == 'float' else str
    return {'default': literal(default), 'range': (low, high, step)}


def load(root):
    data = yaml.safe_load((root / 'config/motion/motion.yaml').read_text())
    if not isinstance(data, dict) or data.get('name') != 'motion' or set(data) - {'name', 'detection', 'recording'}:
        raise ValueError('motion.yaml must have name: motion and detection and recording groups only')
    specs = {}
    for group in ('detection', 'recording'):
        entries = data.get(group)
        if not isinstance(entries, dict):
            raise ValueError(f'motion.yaml: {group} group is required')
        expected = {name for name, (where, _) in SETTINGS.items() if where == group}
        if set(entries) != expected:
            raise ValueError(f'motion.yaml {group}: missing {sorted(expected - set(entries))}, unknown {sorted(set(entries) - expected)}')
        specs.update(entries)
    return {name: setting(name, specs[name], kind) for name, (_, kind) in SETTINGS.items()}


def generate(root):
    settings = load(root)
    lines = ['// Generated from config/motion/motion.yaml; do not edit.', '#pragma once', '',
             'namespace motion_config {',
             'struct Range { double min, max, step; };']
    for name, (_, kind) in SETTINGS.items():
        spec = settings[name]
        lines.append(f'constexpr {kind} {name} = {spec["default"]};')
        if 'range' in spec:
            lines.append(f'constexpr Range {name}_range{{{", ".join(repr(float(v)) for v in spec["range"])}}};')
        if 'options' in spec:
            lines.append(f'constexpr int {name}_options[] = {{{", ".join(map(str, spec["options"]))}}};')
    lines.append('}  // namespace motion_config')
    return '\n'.join(lines) + '\n'


def write(root, output):
    """Write the header only when it changes, so builds are not redone needlessly."""
    text = generate(Path(root))
    output = Path(output)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    write(*map(Path, sys.argv[1:]))
