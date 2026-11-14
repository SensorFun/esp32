const paths = {
  arrow: 'M5 12h14m-6-6 6 6-6 6',
  back: 'M19 12H5m6-6-6 6 6 6',
  external: 'M14 4h6v6m0-6-10 10M10 4H4v16h16v-6',
  grid: 'M4 4h6v6H4zm10 0h6v6h-6zM4 14h6v6H4zm10 0h6v6h-6z',
  camera: 'M4 7h11v11H4zM15 11l5-3v9l-5-3',
  settings: 'M4 7h16M4 17h16M9 4v6m6 4v6',
  signal: 'M5 15a10 10 0 0 1 14 0M2 11a14 14 0 0 1 20 0M8 18a5 5 0 0 1 8 0M12 21h.01',
  book: 'M12 6c-3-2-6-2-9-1v14c3-1 6-1 9 1m0-14c3-2 6-2 9-1v14c-3-1-6-1-9 1M12 6v14',
  chip: 'M7 7h10v10H7zM9 3v4m6-4v4M9 17v4m6-4v4M3 9h4m-4 6h4m10-6h4m-4 6h4',
  check: 'm5 12 4 4L19 6',
  plus: 'M12 5v14M5 12h14',
};
export function Icon({ name = 'arrow', size = 20, ...props }) {
  return <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true" {...props}><path d={paths[name] || paths.arrow} /></svg>;
}
export function BrandMark() {
  return <span className="brand-mark" aria-hidden="true"><i /><i /><i /><i /></span>;
}
