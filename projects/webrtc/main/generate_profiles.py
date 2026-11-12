"""Generate sensor defaults from the workspace YAML, failing on unknown controls."""
from pathlib import Path
import sys
import yaml
root, output = map(Path, sys.argv[1:])
controls = [
    ('brightness', 'brightness', -2, 2), ('contrast', 'contrast', -2, 2),
    ('saturation', 'saturation', -2, 2), ('sharpness', 'sharpness', -3, 3),
    ('denoise', 'denoise', 0, 8), ('jpeg_quality', 'quality', 2, 63),
    ('gain_ceiling', 'gainceiling', 0, 6), ('colorbar', 'colorbar', 0, 1),
    ('horizontal_mirror', 'hmirror', 0, 1), ('vertical_flip', 'vflip', 0, 1),
    ('automatic_white_balance', 'awb', 0, 1), ('awb_gain', 'awb_gain', 0, 1),
    ('white_balance_mode', 'wb_mode', 0, 4), ('automatic_gain', 'agc', 0, 1),
    ('agc_gain', 'agc_gain', 0, 30), ('automatic_exposure', 'aec', 0, 1),
    ('aec_value', 'aec_value', 0, 1200), ('ae_level', 'ae_level', -2, 2),
    ('aec2', 'aec2', 0, 1), ('downsize', 'dcw', 0, 1),
    ('black_pixel_correction', 'bpc', 0, 1), ('white_pixel_correction', 'wpc', 0, 1),
    ('raw_gamma', 'raw_gma', 0, 1), ('lens_correction', 'lenc', 0, 1),
]
lines = ['// Generated from config/frame_grabber; do not edit.', 'static Control controls[] = {']
for name, member, low, high in controls:
    method = {'awb': 'whitebal', 'agc': 'gain_ctrl', 'aec': 'exposure_ctrl'}.get(member, member)
    setter = f'[](sensor_t *s, int v) {{ return s->set_{method}(s, v); }}'
    if member == 'gainceiling':
        setter = '[](sensor_t *s, int v) { return s->set_gainceiling(s, static_cast<gainceiling_t>(v)); }'
    lines.append(f'{{"{name}", {low}, {high}, {setter}, [](sensor_t *s) {{ return int(s->status.{member}); }}}},')
lines += ['};', 'struct Default { const char *name; int value; };']
for sensor in ('ov2640', 'ov3660'):
    profile = yaml.safe_load((root / 'config/frame_grabber' / f'{sensor}.yaml').read_text())
    timing_names = ('high_light', 'balanced', 'low_light')
    if profile.get('timing_profile') not in (None, *timing_names):
        raise ValueError('Unknown timing profile')
    unknown = set(profile) - {x[0] for x in controls} - {'sensor', 'timing_profile'}
    if unknown:
        raise ValueError(f'Unmapped camera settings: {unknown}')
    lines.append(f'static const Default defaults_{sensor}[] = {{')
    if 'timing_profile' in profile:
        lines.append('{"timing_profile", ' + str(timing_names.index(profile['timing_profile'])) + '},')
    for name, _, low, high in controls:
        if name in profile:
            value = profile[name]
            if type(value) is not int or not low <= value <= high:
                raise ValueError(f'{sensor}.{name} out of range')
            lines.append(f'{{"{name}", {value}}},')
    lines.append('};')
output.write_text('\n'.join(lines) + '\n')
