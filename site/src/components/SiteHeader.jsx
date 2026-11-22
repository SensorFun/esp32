import { sitePath } from '../routes.mjs';
// The SensorFun landing page lists every project space, including ESP32.
export const siteHome = 'https://sensorfun.net/';

export function SiteHeader({ project, children }) {
  return <header className="sensorfan-header"><a href={siteHome} className="sensorfan-brand"><img src={sitePath('/logo.png')} alt="" /><span>SensorFun</span></a><nav className="sensorfan-trail" aria-label="Breadcrumb"><a href={sitePath('/')} aria-current={project ? undefined : 'page'}>ESP32</a>{project && <><span aria-hidden="true">/</span><a href={sitePath(`/${project.id === 'flasher' ? 'tools' : 'projects'}/${project.id}`)} aria-current="page">{project.name}</a></>}</nav>{children}</header>;
}
