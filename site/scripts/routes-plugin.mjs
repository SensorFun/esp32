// Path routing on a static host (GitHub Pages): every route gets its own copy
// of index.html as <route>.html, which GitHub Pages serves at the clean URL
// (/projects/webrtc, no redirect or trailing slash), and 404.html renders the
// app's "Page not found" for anything else.
import { routes } from '../src/projects/catalog.mjs';

export const routeFile = route => `${route.slice(1)}.html`;

export function routesPlugin() {
  return {
    name: 'route-pages',
    apply: 'build',
    enforce: 'post',  // After Vite emits index.html.
    generateBundle(_, bundle) {
      const page = bundle['index.html'];
      if (!page || page.type !== 'asset') throw Error('index.html missing from the build');
      for (const route of routes.filter(item => item !== '/')) {
        this.emitFile({ type: 'asset', fileName: routeFile(route), source: page.source });
      }
      this.emitFile({ type: 'asset', fileName: '404.html', source: page.source });
    },
  };
}
