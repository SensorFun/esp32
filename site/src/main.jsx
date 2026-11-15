import { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { projects } from './projects';
import { SiteHeader, siteHome } from './components/SiteHeader';
import './styles.css';
import './sensorfan.css';

const strip = route => route.replace(/^\/spaces(?=\/|$)/, '/projects').replace(/(.)\/$/, '$1');

function App() {
  const readRoute = () => strip(window.location.hash.slice(1) || '/');
  const [route, setRoute] = useState(readRoute);
  useEffect(() => {
    const update = () => { setRoute(readRoute()); window.scrollTo(0, 0); };
    window.addEventListener('hashchange', update);
    return () => window.removeEventListener('hashchange', update);
  }, []);
  const project = projects.find(item => route === `/projects/${item.id}`);
  const home = route === '/' || route === '/projects';
  useEffect(() => { document.title = project ? `${project.name} · ESP32 · SensorFun` : home ? 'ESP32 · SensorFun' : 'Page not found · SensorFun'; }, [project, home]);
  const Project = project?.component;
  if (Project) return <Project />;
  return <div className="rtc-theme sensorfan"><div className="rtc-body"><SiteHeader /><main id="main-content" tabIndex="-1" className="rtc-content">{home ? <>
    <div className="sensorfan-intro"><h1>ESP32 projects</h1><p>Choose a project to get started.</p></div>
    <div className="sensorfan-projects">{projects.map(item => <a className="rtc-panel sensorfan-project" key={item.id} href={`#/projects/${item.id}`}><span className="pill">{item.category}</span><h2>{item.name}</h2><p>{item.description}</p><span className="sensorfan-project-open">Open {item.name} →</span></a>)}</div>
  </> : <>
    <div className="sensorfan-intro"><h1>Page not found</h1><p>Return to the ESP32 projects or to SensorFun.</p></div>
    <div className="sensorfan-actions"><a className="button rtc-primary" href="#/">ESP32 projects</a><a className="button sensorfan-secondary" href={siteHome}>All SensorFun spaces</a></div>
  </>}</main></div></div>;
}

createRoot(document.getElementById('root')).render(<><a className="skip-link" href="#main-content" onClick={event => { event.preventDefault(); document.getElementById('main-content')?.focus(); }}>Skip to content</a><App /></>);
