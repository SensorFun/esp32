// Shared by the browser and bridge. TURN and TURN credentials never reach ICE.
export function stunOnlyServers(servers) {
  if (!Array.isArray(servers)) throw new TypeError('Invalid ICE server configuration');
  return servers.flatMap(server => {
    if (!server || typeof server !== 'object') return [];
    const values = Array.isArray(server.urls) ? server.urls : [server.urls];
    const urls = values.filter(url => typeof url === 'string' && /^stun:[^\s]+$/i.test(url));
    return urls.length ? [{ urls }] : [];
  });
}
