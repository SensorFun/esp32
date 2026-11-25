"""Generate per-sensor camera control tables from the workspace YAML.

config/frame_grabber/<family>.yaml lists sensors and may hold a `common` section
shared by all of them; config/frame_grabber/sensors/<sensor>.yaml adds each
sensor's own controls and overrides any shared key or control it repeats.
Every listed sensor is embedded: the firmware detects the installed one at
runtime and offers exactly its controls. Unknown controls, malformed value
sets and out-of-range defaults fail the build rather than being dropped.
"""
from pathlib import Path
import json
import sys
import yaml

# Control name -> esp32-camera sensor_t status member (setters derive from it).
MEMBERS = {
    'brightness': 'brightness', 'contrast': 'contrast', 'saturation': 'saturation',
    'sharpness': 'sharpness', 'denoise': 'denoise', 'jpeg_quality': 'quality',
    'gain_ceiling': 'gainceiling', 'colorbar': 'colorbar', 'horizontal_mirror': 'hmirror',
    'vertical_flip': 'vflip', 'automatic_white_balance': 'awb', 'awb_gain': 'awb_gain',
    'white_balance_mode': 'wb_mode', 'automatic_gain': 'agc', 'agc_gain': 'agc_gain',
    'automatic_exposure': 'aec', 'aec_value': 'aec_value', 'ae_level': 'ae_level',
    'aec2': 'aec2', 'downsize': 'dcw', 'black_pixel_correction': 'bpc',
    'white_pixel_correction': 'wpc', 'raw_gamma': 'raw_gma', 'lens_correction': 'lenc',
}
# Sensors the firmware can detect (camera_controls.cpp) and their profiles.
SENSORS = ('ov2640', 'ov3660', 'gc0308')
TIMING = ('high_light', 'mid_light', 'low_light')
# Sensors with OV light timing (CLKRC or HTS line period control) and clock_2x.
TIMED = {'ov2640', 'ov3660'}
# Keys a sensor profile (shared section merged with the sensor file) may hold.
PROFILE_KEYS = {'timing_profile', 'clock_2x', 'default_resolution', 'controls'}
# Largest resolution each sensor offers (camera_controls.cpp max_resolution()).
SENSOR_MAX = {'ov2640': '1600x1200', 'ov3660': '2048x1536', 'gc0308': '640x480'}
# esp32-camera 2.1.6 framesize_t names by resolution, in the driver's order.
RESOLUTIONS = {
    '96x96': '96X96', '160x120': 'QQVGA', '128x128': '128X128', '176x144': 'QCIF',
    '240x176': 'HQVGA', '240x240': '240X240', '320x240': 'QVGA', '320x320': '320X320',
    '400x296': 'CIF', '480x320': 'HVGA', '640x480': 'VGA', '800x600': 'SVGA',
    '1024x768': 'XGA', '1280x720': 'HD', '1280x1024': 'SXGA', '1600x1200': 'UXGA',
    '1920x1080': 'FHD', '720x1280': 'P_HD', '864x1536': 'P_3MP', '2048x1536': 'QXGA',
}


def integer(value):
    return type(value) is int


def label(value):
    return f'+{value}' if value > 0 else str(value)


def control_spec(sensor, name, spec):
    where = f'{sensor}.{name}'
    if not isinstance(spec, dict) or not integer(spec.get('default')):
        raise ValueError(f'{where}: needs an integer default')
    kinds = [key for key in ('switch', 'options', 'range') if key in spec]
    if len(kinds) != 1 or set(spec) - {'default', kinds[0]}:
        raise ValueError(f'{where}: give exactly one of switch, options or range')
    kind, default = kinds[0], spec['default']
    if kind == 'switch':
        if spec['switch'] is not True:
            raise ValueError(f'{where}: switch must be true')
        options = [(0, 'Off'), (1, 'On')]
    elif kind == 'options':
        values = spec['options']
        if isinstance(values, list):
            options = [(value, label(value)) if integer(value) else (None, None) for value in values]
        elif isinstance(values, dict):
            options = list(values.items())
        else:
            options = []
        if len(options) < 2 or any(not integer(v) or not isinstance(text, str) or not text for v, text in options):
            raise ValueError(f'{where}: options must be two or more integers, or integer: text labels')
        if [v for v, _ in options] != sorted({v for v, _ in options}):
            raise ValueError(f'{where}: options must be unique and ascending')
    else:
        bounds = spec['range']
        if not (isinstance(bounds, list) and len(bounds) == 2 and all(map(integer, bounds)) and bounds[0] < bounds[1]):
            raise ValueError(f'{where}: range must be [min, max] integers')
        options = []
    low, high = (options[0][0], options[-1][0]) if options else spec['range']
    allowed = [v for v, _ in options] if options else range(low, high + 1)
    if default not in allowed:
        raise ValueError(f'{where}: default {default} is not an allowed value')
    return {'name': name, 'kind': kind, 'default': default, 'low': low, 'high': high, 'options': options}


def load_family(root, family):
    listed = yaml.safe_load((root / 'config/frame_grabber' / f'{family}.yaml').read_text())
    if (not isinstance(listed, dict) or listed.get('name') != family or not isinstance(listed.get('sensors'), list)
            or set(listed) - {'name', 'sensors', 'common'}):
        raise ValueError(f'{family}.yaml must have name: {family}, a sensors list and an optional common section only')
    common = listed.get('common', {})
    if not isinstance(common, dict) or set(common) - PROFILE_KEYS or not isinstance(common.get('controls', {}), dict):
        raise ValueError(f'{family}.yaml: common may hold only {sorted(PROFILE_KEYS)}, with controls as a mapping')
    return listed['sensors'], common


def load_sensor(root, sensor, common):
    own = yaml.safe_load((root / 'config/frame_grabber/sensors' / f'{sensor}.yaml').read_text())
    if not isinstance(own, dict) or own.get('name') != sensor:
        raise ValueError(f'sensors/{sensor}.yaml: name must be {sensor}')
    if set(own) - PROFILE_KEYS - {'name'}:
        raise ValueError(f'{sensor}: unknown keys {set(own) - PROFILE_KEYS - {"name"}}')
    if not isinstance(own.get('controls', {}), dict):
        raise ValueError(f'{sensor}: controls must be a mapping')
    # The sensor file overrides shared keys; nested settings (controls,
    # clock_2x) merge one level deep, so it can override single entries.
    profile = dict(common)
    for key, value in own.items():
        profile[key] = {**common[key], **value} if isinstance(common.get(key), dict) and isinstance(value, dict) else value
    profile.setdefault('controls', {})
    # Double the sensor clock (OV2640 CLKRC doubler, OV3660 PLL pre-divider)
    # up to a maximum resolution.
    clock = profile.get('clock_2x', {'enabled': False})
    if (not isinstance(clock, dict) or set(clock) - {'enabled', 'max_resolution'} or not isinstance(clock.get('enabled'), bool)
            or ('max_resolution' in clock and clock['max_resolution'] not in RESOLUTIONS)
            or (clock['enabled'] and (sensor not in TIMED or 'max_resolution' not in clock))):
        raise ValueError(f'{sensor}: clock_2x must be {{enabled: true|false, max_resolution: WIDTHxHEIGHT}}; only OV sensors '
                         f'support it, and max_resolution is one of {", ".join(RESOLUTIONS)}')
    clock_2x = clock['enabled']
    # Stream resolution before saved settings, within the sensor's sizes.
    default = profile.get('default_resolution')
    order = list(RESOLUTIONS)
    if default not in RESOLUTIONS or order.index(default) > order.index(SENSOR_MAX[sensor]):
        raise ValueError(f'{sensor}: default_resolution must be WIDTHxHEIGHT up to {SENSOR_MAX[sensor]}')
    clock_max = 'FRAMESIZE_' + RESOLUTIONS[clock['max_resolution']] if clock_2x else 'FRAMESIZE_INVALID'
    timing = profile.get('timing_profile')
    if (sensor in TIMED) != (timing is not None) or (timing is not None and timing not in TIMING):
        raise ValueError(f'{sensor}: timing_profile must be one of {TIMING} for OV sensors and absent otherwise')
    controls = profile.get('controls')
    if not isinstance(controls, dict) or not controls:
        raise ValueError(f'{sensor}: controls are required')
    unknown = set(controls) - set(MEMBERS)
    if unknown:
        raise ValueError(f'{sensor}: unmapped camera controls {sorted(unknown)}')
    specs = [control_spec(sensor, name, controls[name]) for name in MEMBERS if name in controls]
    return {'sensor': sensor, 'timing': TIMING.index(timing) if timing else -1, 'clock_2x': clock_2x,
            'clock_max': clock_max, 'default_resolution': 'FRAMESIZE_' + RESOLUTIONS[default], 'controls': specs}


def generate(root):
    shared = {}
    sensors = []
    for family in ('ov', 'gc'):
        listed, common = load_family(root, family)
        sensors += listed
        shared.update({sensor: common for sensor in listed if isinstance(sensor, str)})
    if sorted(sensors) != sorted(SENSORS):
        raise ValueError(f'Frame grabbers must list each detectable sensor once: {SENSORS}')
    profiles = [load_sensor(root, sensor, shared[sensor]) for sensor in SENSORS]
    lines = ['// Generated from config/frame_grabber; do not edit.', 'static Control controls[] = {']
    for name, member in MEMBERS.items():
        method = {'awb': 'whitebal', 'agc': 'gain_ctrl', 'aec': 'exposure_ctrl'}.get(member, member)
        setter = f'[](sensor_t *s, int v) {{ return s->set_{method}(s, v); }}'
        if member == 'gainceiling':
            setter = '[](sensor_t *s, int v) { return s->set_gainceiling(s, static_cast<gainceiling_t>(v)); }'
        lines.append(f'{{"{name}", {setter}, [](sensor_t *s) {{ return int(s->status.{member}); }}}},')
    lines += ['};',
              'struct Option { int value; const char *label; };',
              'enum class Kind { range, list, toggle };',
              'struct ControlSpec { const char *name; Kind kind; int value, low, high; const Option *options; size_t option_count; };',
              'struct SensorProfile { const char *sensor; int timing_profile; bool clock_2x; framesize_t clock_2x_max; framesize_t default_resolution; const ControlSpec *controls; size_t count; };']
    kinds = {'range': 'Kind::range', 'options': 'Kind::list', 'switch': 'Kind::toggle'}
    for profile in profiles:
        for spec in profile['controls']:
            if spec['options']:
                entries = ', '.join(f'{{{value}, {json.dumps(text)}}}' for value, text in spec['options'])
                lines.append(f'static const Option options_{profile["sensor"]}_{spec["name"]}[] = {{{entries}}};')
        lines.append(f'static const ControlSpec controls_{profile["sensor"]}[] = {{')
        for spec in profile['controls']:
            options = f'options_{profile["sensor"]}_{spec["name"]}' if spec['options'] else 'nullptr'
            lines.append(f'{{"{spec["name"]}", {kinds[spec["kind"]]}, {spec["default"]}, {spec["low"]}, {spec["high"]}, {options}, {len(spec["options"])}}},')
        lines.append('};')
    lines.append('static const SensorProfile profiles[] = {')
    for profile in profiles:
        sensor = profile['sensor']
        lines.append(f'{{"{sensor}", {profile["timing"]}, {str(profile["clock_2x"]).lower()}, {profile["clock_max"]}, {profile["default_resolution"]}, controls_{sensor}, sizeof(controls_{sensor}) / sizeof(ControlSpec)}},')
    lines.append('};')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    root, output = map(Path, sys.argv[1:])
    output.write_text(generate(root))
