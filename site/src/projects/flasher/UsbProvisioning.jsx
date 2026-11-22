import { useEffect, useRef, useState } from 'react';
import { firmwares } from 'virtual:board-catalog';
import { openUsbSession, usbSetupApi } from './usbSetup.mjs';

// The project's own setup page (config/firmware/<project>.yaml provisioning.page),
// with fetch routed to the USB session.
const shim = '<script>window.fetch=(path,options)=>frameElement.usbFetch(String(path),options);</script>';
const withShim = page => page.replace('<script>', `${shim}<script>`);
const projectName = id => firmwares.find(item => item.id === id)?.name ?? id;

// Why a board cannot be provisioned for `project`, or '' when it can.
export function firmwareMismatch(project, reported) {
  if (typeof reported !== 'string' || !reported) {
    return `This board's firmware does not say which project it runs. Flash ${project.name} first, then select Provision again.`;
  }
  if (reported === project.id) return '';
  const running = projectName(reported);
  return `This board runs ${running} firmware, but ${project.name} is selected. Select ${running} as the firmware source to provision it, or flash ${project.name} first.`;
}

// Popup with the same setup page the board serves on its setup access point,
// talking to the board over USB instead.
export function UsbProvisioning({ port, project, onClose, onViewer }) {
  const dialog = useRef(null);
  const frame = useRef(null);
  const [page, setPage] = useState('');
  const [error, setError] = useState('');

  useEffect(() => {
    dialog.current?.showModal();
    const controller = new AbortController();
    let session, viewer = '';
    (async () => {
      try {
        const [source, opened] = await Promise.all([project.provisioning.page(), openUsbSession(port, controller.signal)]);
        session = opened;
        if (controller.signal.aborted) return;
        const mismatch = firmwareMismatch(project, session.firmware.project);
        if (mismatch) { setError(mismatch); await session.close(); return; }
        frame.current.usbFetch = usbSetupApi(session, {
          onConnected: url => { viewer = url; onViewer(url); },
          onFinish: () => {
            // Runs inside the click on the page's camera link, so the new tab is allowed.
            if (viewer) { const tab = window.open(viewer, '_blank'); if (tab) tab.opener = null; }
            onClose();
          },
        });
        setPage(withShim(source));
      } catch (failure) {
        if (!controller.signal.aborted) setError(failure.message);
      }
    })();
    return () => { controller.abort(); void session?.close(); };
  }, [port, project]);

  return <dialog ref={dialog} className="flasher-provisioning" aria-labelledby="usb-provisioning-title" onClose={onClose}
    onCancel={event => { event.preventDefault(); onClose(); }}>
    <header>
      <h2 id="usb-provisioning-title">Provision {project.name}</h2>
      <button type="button" className="button rtc-secondary" onClick={onClose}>Close</button>
    </header>
    {!page && !error && <p role="status">Connecting to the board over USB…</p>}
    {error && <p role="alert" className="flasher-error">{error}</p>}
    <iframe ref={frame} title={`${project.name} setup`} srcDoc={page || undefined} hidden={!page} />
  </dialog>;
}
