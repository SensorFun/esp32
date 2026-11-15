import { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { projects } from './projects';
import './styles.css';
import './sensorfan.css';

function App() {
  const readRoute = () => (window.location.hash.slice(1) || '/').replace(/^\/spaces(?=\/|$)/, '/projects');
  const [route, setRoute] = useState(readRoute);
  useEffect(() => {
    const update = () => { setRoute(readRoute()); window.scrollTo(0, 0); };
    window.addEventListener('hashchange', update);
    return () => window.removeEventListener('hashchange', update);
  }, []);
  const project = projects.find(item => route === `/projects/${item.id}` || route === `/projects/${item.id}/`);
  useEffect(() => { document.title = project ? `${project.name} · SensorFan` : 'SensorFan'; }, [project]);
  const Project = project?.component;
  if (Project) return <Project />;
  const home = route === '/' || route === '/projects';
  return <div className="rtc-theme sensorfan"><div className="rtc-body"><header className="sensorfan-header"><a href="#/" className="sensorfan-brand"><img src="./logo.png" alt="" /><span>SensorFan</span></a></header><main id="main-content" tabIndex="-1" className="rtc-content"><div className="sensorfan-intro"><h1>{home ? 'Projects' : 'Project not found'}</h1><p>{home ? 'Choose a project to get started.' : 'Return home to choose an available project.'}</p></div>{home ? <div className="sensorfan-projects">{projects.map(item => <a className="rtc-panel sensorfan-project" key={item.id} href={`#/projects/${item.id}`}><span className="pill">{item.category}</span><h2>{item.name}</h2><p>{item.description}</p><span className="sensorfan-project-open">Open {item.name} →</span></a>)}</div> : <a className="button rtc-primary" href="#/">Back to projects</a>}</main></div></div>;
}

createRoot(document.getElementById('root')).render(<><a className="skip-link" href="#main-content" onClick={event => { event.preventDefault(); document.getElementById('main-content')?.focus(); }}>Skip to content</a><App /></>);
