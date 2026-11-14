import { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { Home } from './pages/Home';
import { spaces } from './spaces';
import './styles.css';

function getRoute() {
  return (window.location.hash.slice(1) || '/').split('/').filter(Boolean);
}
function App() {
  const [route, setRoute] = useState(getRoute);
  useEffect(() => {
    const update = () => setRoute(getRoute());
    window.addEventListener('hashchange', update);
    return () => window.removeEventListener('hashchange', update);
  }, []);
  const space = route[0] === 'spaces' && route[1] ? spaces.find(s => s.id === route[1]) : null;
  const knownHome = route.length === 0 || (route.length === 1 && ['spaces', 'about'].includes(route[0]));
  useEffect(() => {
    document.title = space ? `${space.name} · Device Spaces` : 'Device Spaces';
    document.querySelector('meta[name="theme-color"]').content = space ? '#101713' : '#f3f5ef';
    window.scrollTo(0, 0);
    document.getElementById('main-content')?.focus({ preventScroll: true });
  }, [route, space]);
  const Space = space?.component;
  return <><a className="skip-link" href="#main-content" onClick={e => { e.preventDefault(); document.getElementById('main-content')?.focus(); }}>Skip to content</a>{Space ? <Space subpage={route.slice(2).join('/')} /> : knownHome ? <Home page={route[0]} /> : <div className="not-found"><main id="main-content" tabIndex="-1"><p className="eyebrow">DEVICE SPACES</p><h1>This space isn’t here.</h1><p>Choose an available project from the collection.</p><a className="button home-primary" href="#/spaces">Find a project space →</a></main></div>}</>;
}
createRoot(document.getElementById('root')).render(<App />);
