// Path routing: /projects/webrtc rather than #/projects/webrtc. The base comes
// from Vite (`--base` at build time), so the site also works under a subfolder.
export const base = import.meta.env.BASE_URL;

// '/projects/webrtc' → '<base>projects/webrtc'.
export const sitePath = route => base + route.replace(/^\//, '');

// The current route without the base: '/projects/webrtc'.
export function currentRoute() {
  const path = window.location.pathname;
  const inside = path.startsWith(base) ? path.slice(base.length) : path.replace(/^\//, '');
  return '/' + inside.replace(/index\.html$/, '');
}

export const routeChange = 'sensorfun-route';
export function navigate(url) {
  window.history.pushState(null, '', url);
  window.dispatchEvent(new Event(routeChange));
}

// Same-tab clicks on links inside the site change the route without a reload.
export function interceptLinks(event) {
  if (event.defaultPrevented || event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
  const link = event.target.closest?.('a[href]');
  if (!link || link.target || link.hasAttribute('download')) return;
  const url = new URL(link.href, window.location.href);
  if (url.origin !== window.location.origin || !url.pathname.startsWith(base)) return;
  if (url.pathname === window.location.pathname && url.search === window.location.search && url.hash) return;
  if (url.pathname.startsWith(base + 'firmware/') || /\.[a-z0-9]+$/i.test(url.pathname)) return;
  event.preventDefault();
  navigate(url.pathname + url.search + url.hash);
}
