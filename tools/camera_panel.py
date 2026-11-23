"""Drive the shared camera settings panel (projects/common/camera_ui/settings.js).

The board's local viewer and the website render the same component; these
helpers work on either through Playwright, as a user would.
"""
from playwright.sync_api import expect

ROLES = {'switch': 'switch', 'list': 'combobox', 'range': 'spinbutton'}


def title(control):
    return control['name'].replace('_', ' ')


def field(panel, control):
    return panel.get_by_role(ROLES[control['kind']], name=title(control), exact=True)


def slider_stops(control):
    """The 11 values a range control's 10-step slider moves between."""
    low, high = control['min'], control['max']
    return [int(low + i * (high - low) / 10 + 0.5) for i in range(11)]


def slider(panel, control):
    return panel.get_by_role('slider', name=title(control), exact=True)


def check_settings_panel(panel, settings):
    """Each control the camera reports appears once, as its kind: On/Off
    switches, lists of the camera's labelled values, and for wide ranges a
    10-step slider beside an exact number field. Returns the number of each kind."""
    counts = {'switch': 0, 'list': 0, 'range': 0}
    for control in settings['controls']:
        assert control['kind'] in ROLES, control
        counts[control['kind']] += 1
        box = field(panel, control)
        expect(box).to_have_count(1)
        if control['kind'] == 'switch':
            assert (control['min'], control['max']) == (0, 1), control
            (expect(box).to_be_checked if control['value'] == 1 else expect(box).not_to_be_checked)()
        elif control['kind'] == 'list':
            options = box.locator('option').evaluate_all('options=>options.map(o=>[Number(o.value),o.textContent])')
            assert options == [list(option) for option in control['options']], (control['name'], options)
            assert len(options) >= 2 and options[0][0] == control['min'] and options[-1][0] == control['max'], control
            expect(box).to_have_value(str(control['value']))
        else:
            assert box.get_attribute('min') == str(control['min']) and box.get_attribute('max') == str(control['max']), control
            expect(box).to_have_value(str(control['value']))
            bar = slider(panel, control)
            expect(bar).to_have_count(1)
            assert (bar.get_attribute('min'), bar.get_attribute('max'), bar.get_attribute('step')) == ('0', '10', '1'), control
            stops = slider_stops(control)
            nearest = min(range(11), key=lambda i: (abs(stops[i] - control['value']), i))
            expect(bar).to_have_value(str(nearest))
    # No free-form input for controls with few values.
    expect(panel.get_by_role('spinbutton')).to_have_count(counts['range'])
    expect(panel.get_by_role('switch')).to_have_count(counts['switch'])
    expect(panel.get_by_role('slider')).to_have_count(counts['range'])
    return counts


def allowed_values(control):
    if control['kind'] == 'switch':
        return [0, 1]
    if control['kind'] == 'list':
        return [option[0] for option in control['options']]
    return list(range(control['min'], control['max'] + 1))


def slide(page, panel, control, position, wait):
    """Move a range control's slider to one of its 11 positions and return the
    value the camera applied. `wait(expression, arg)` polls the page; the
    page must record replies in window.lastResponse. Only the OV3660 AEC value
    may come back lower: its driver clamps it to the current frame length."""
    page.evaluate('window.lastResponse=null')
    slider(panel, control).fill(str(position))
    requested = slider_stops(control)[position]
    wait("n=>window.lastResponse?.controls?.some(c=>c.name===n)", control['name'])
    reported = page.evaluate("n=>window.lastResponse.controls.find(c=>c.name===n).value", control['name'])
    assert reported == requested or (control['name'] == 'aec_value' and 0 <= reported < requested), (control['name'], requested, reported)
    # The panel shows what the camera applied, and the slider follows it.
    expect(field(panel, control)).to_have_value(str(reported))
    return reported


def set_control(panel, control, value):
    """Change one control the way its kind is used: toggle, choose or type."""
    box = field(panel, control)
    if control['kind'] == 'switch':
        box.set_checked(bool(value))
    elif control['kind'] == 'list':
        box.select_option(str(value))
    else:
        box.fill(str(value))
