// Shared by the board's local viewer and the React website.
class CameraSettings extends HTMLElement {
  constructor() { super(); this.attachShadow({ mode: 'open' }); }
  set settings(value) {
    this.value = value;
    if (this.dirty && !value.saved) return;
    this.dirty = false; this.pending = false;
    this.render(value.saved ? 'Settings saved on camera.' : 'Changes take effect immediately. Select Save to keep them after reboot.');
  }
  set error(message) {
    this.shadowRoot.querySelectorAll('button').forEach(button => { button.disabled = false; });
    if (this.pending) this.shadowRoot.querySelectorAll('input,select').forEach(input => { input.disabled = input.dataset.wasDisabled === 'true'; });
    this.pending = false;
    const status = this.shadowRoot.querySelector('[role=status]');
    if (status) status.textContent = message;
  }
  render(message) {
    this.shadowRoot.innerHTML = `<style>
      :host{display:block;color:#e4eadd;font:14px system-ui,sans-serif}:host([hidden]){display:none}*{box-sizing:border-box}
      section{background:#17201a;border:1px solid #2a352c;border-radius:8px;padding:24px;min-width:0}
      h2{font-size:20px;font-weight:500;margin:0 0 20px}.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,240px),1fr));gap:16px}
      label{display:flex;flex-direction:column;gap:7px;min-width:0}label.counter{flex-direction:row;align-items:center;justify-content:space-between;gap:12px;align-self:end}label.counter span{min-width:0;overflow-wrap:anywhere}label.counter input{width:88px;flex:none;text-align:right;font-variant-numeric:tabular-nums}input,select,button{font:inherit;min-height:44px;border-radius:6px;padding:10px;border:1px solid #415335}
      input,select{background:#0d180e;color:#d8e8cb;width:100%;min-width:0}input:disabled{opacity:.45}
      .actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:24px}.actions:first-child{margin:0 0 24px}button{cursor:pointer;background:#00d4cb;color:#07302c;font-weight:600}button:last-child{background:transparent;color:#d8e8cb}button:disabled{opacity:.5;cursor:wait}
      [role=status]{margin:16px 0 0;line-height:1.6;color:#b7c9ab}input:focus-visible,select:focus-visible,button:focus-visible{outline:2px solid #00d4cb;outline-offset:3px}
      @media(max-width:600px){section{padding:18px}input,select{font-size:16px}.actions button{flex:1}}
    </style><section><h2>Camera settings</h2><form><div class="actions"><button type="submit">Save</button><button type="button">Reset to Default</button></div><div class="fields"></div><div class="actions"><button type="submit">Save</button><button type="button">Reset to Default</button></div><p role="status"></p></form></section>`;
    this.shadowRoot.querySelector('h2').textContent = `Camera settings · ${this.value.sensor}`;
    const fields = this.shadowRoot.querySelector('.fields');
    const add = (name, title, control) => {
      const label = document.createElement('label');
      const text = document.createElement('span'); text.textContent = title;
      // Number fields hold one to four digits: keep them on the label's line.
      if (control.type === 'number') label.className = 'counter';
      control.name = name; control.setAttribute('aria-label', title);
      label.append(text, control); fields.append(label);
      control.addEventListener('input', () => {
        this.dirty = true;
        if (!control.checkValidity() || control.value === '') return;
        this.shadowRoot.querySelector('[role=status]').textContent = 'Live changes. Select Save to keep them after reboot.';
        this.dispatchEvent(new CustomEvent('camera-command', {
          detail: { op: 'set', name, value: name === 'timing_profile' ? control.value : Number(control.value) },
          bubbles: true, composed: true,
        }));
      });
    };
    const select = (name, title, options, value) => {
      const control = document.createElement('select');
      for (const [id, text] of options) { const option = document.createElement('option'); option.value = id; option.textContent = text; control.append(option); }
      control.value = value; add(name, title, control);
    };
    select('resolution', 'Resolution', this.value.resolutions.map(size => [size.value, `${size.width} × ${size.height}`]), this.value.resolution);
    if (this.value.timing_profiles.length) select('timing_profile', 'Light timing', this.value.timing_profiles.map(name => [name, ({high_light:'High light',balanced:'Mid light',low_light:'Low light'})[name]]), this.value.timing_profile);
    for (const c of this.value.controls) {
      const input = document.createElement('input'); input.type = 'number'; input.min = c.min; input.max = c.max; input.step = 1; input.value = c.value; input.disabled = !c.supported; input.required = c.supported;
      add(c.name, c.name.replaceAll('_', ' '), input);
    }
    this.shadowRoot.querySelector('[role=status]').textContent = message;
    const send = detail => {
      if (this.pending) return;
      this.pending = true; this.dirty = true;
      this.shadowRoot.querySelectorAll('button').forEach(button => { button.disabled = true; });
      this.shadowRoot.querySelectorAll('input,select').forEach(input => { input.dataset.wasDisabled = String(input.disabled); input.disabled = true; });
      this.shadowRoot.querySelector('[role=status]').textContent = 'Saving on camera…';
      this.dispatchEvent(new CustomEvent('camera-command', { detail, bubbles: true, composed: true }));
    };
    this.shadowRoot.querySelector('form').onsubmit = event => {
      event.preventDefault();
      const settings = {};
      for (const input of this.shadowRoot.querySelectorAll('input:not(:disabled),select')) settings[input.name] = input.name === 'timing_profile' ? input.value : Number(input.value);
      send({ op: 'apply', settings });
    };
    this.shadowRoot.querySelectorAll('button[type=button]').forEach(button => { button.onclick = () => send({ op: 'reset_defaults' }); });
  }
}
customElements.define('sensorfan-camera-settings', CameraSettings);

// Shared tab navigation for the site and the board's local camera viewer.
let cameraTabsId = 0;
class CameraTabs extends HTMLElement {
  constructor() { super(); this.attachShadow({ mode: 'open' }); }
  connectedCallback() {
    queueMicrotask(() => {
      if (!this.isConnected) return;
      const panels = [...this.children], prefix = `camera-tabs-${++cameraTabsId}`;
      this.shadowRoot.innerHTML = `<style>
        :host{display:block;min-width:0;color:inherit;font:14px system-ui,sans-serif}
        nav{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:16px;border-bottom:1px solid #415335;padding-bottom:10px}
        button{flex:1 1 150px;min-width:0;min-height:48px;padding:12px 14px;border:1px solid #415335;border-radius:7px;background:#172418;color:#bdcbb3;font:inherit;font-size:16px;cursor:pointer}
        button[aria-selected=true]{background:#c5ed9f;color:#203316;border-color:#c5ed9f}
        button:focus-visible{outline:3px solid #91cbea;outline-offset:2px}
        ::slotted([hidden]){display:none!important}
      </style><nav role="tablist" aria-label="Camera controls"></nav><slot></slot>`;
      const nav = this.shadowRoot.querySelector('nav');
      const select = index => panels.forEach((panel, i) => {
        panel.hidden = i !== index;
        nav.children[i].setAttribute('aria-selected', String(i === index));
        nav.children[i].tabIndex = i === index ? 0 : -1;
      });
      panels.forEach((panel, index) => {
        const button = document.createElement('button');
        button.type = 'button'; button.textContent = panel.dataset.label;
        button.id = `${prefix}-tab-${index}`; button.setAttribute('role', 'tab');
        panel.id = `${prefix}-panel-${index}`; panel.setAttribute('role', 'tabpanel');
        panel.setAttribute('aria-labelledby', button.id); button.setAttribute('aria-controls', panel.id);
        button.onclick = () => select(index);
        button.onkeydown = event => {
          let next;
          if (event.key === 'ArrowRight') next = (index + 1) % panels.length;
          else if (event.key === 'ArrowLeft') next = (index + panels.length - 1) % panels.length;
          else if (event.key === 'Home') next = 0;
          else if (event.key === 'End') next = panels.length - 1;
          else return;
          event.preventDefault(); select(next); nav.children[next].focus();
        };
        nav.append(button);
      });
      select(0);
    });
  }
}
customElements.define('sensorfan-camera-tabs', CameraTabs);
