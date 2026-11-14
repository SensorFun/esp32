export function HardwareArt({ compact = false }) {
  return <svg className={compact ? 'hardware-art compact' : 'hardware-art'} viewBox="0 0 520 400" role="img" aria-label="Illustration of a camera module on a circuit board">
    <defs><pattern id={compact ? 'dots-small' : 'dots-large'} width="24" height="24" patternUnits="userSpaceOnUse"><circle cx="2" cy="2" r="1" fill="currentColor" opacity=".15" /></pattern></defs>
    <rect width="520" height="400" fill={`url(#${compact ? 'dots-small' : 'dots-large'})`} />
    <g transform="translate(260 200) rotate(-12) translate(-260 -200)">
      <rect x="132" y="56" width="256" height="288" rx="24" fill="#274c3b" stroke="#649873" strokeWidth="2" />
      {[82, 106, 130, 154, 178, 202, 226, 250, 274, 298].map(y => <g key={y}><rect x="120" y={y} width="25" height="9" rx="2" fill="#c5b179" /><rect x="375" y={y} width="25" height="9" rx="2" fill="#c5b179" /></g>)}
      <g stroke="#7ba77c" strokeWidth="2" fill="none" opacity=".6"><path d="M160 100h30v35h30m-60 10h15v70h40m-55 25h38v-30m160-110h-28v35h-24m52 95h-35v-25m-95 95v-38h35v-20m-65 58v-23h-15" /></g>
      <rect x="207" y="93" width="106" height="88" rx="7" fill="#acb9a5" /><path d="M220 107h77m-77 10h77m-77 10h77" stroke="#798a77" strokeWidth="2" />
      <rect x="210" y="185" width="100" height="90" rx="12" fill="#192d26" stroke="#649873" />
      <circle cx="260" cy="230" r="33" fill="#0b1713" stroke="#7b9d82" strokeWidth="5" /><circle cx="260" cy="230" r="22" fill="#203b35" stroke="#3f766a" strokeWidth="2" /><circle cx="260" cy="230" r="11" fill="#0c1d18" /><circle cx="253" cy="222" r="5" fill="#b9e8c6" opacity=".55" />
      <rect x="236" y="317" width="48" height="30" rx="4" fill="#b6beb0" /><rect x="244" y="326" width="32" height="12" rx="3" fill="#263a30" />
      <circle cx="345" cy="293" r="5" fill="#c5f399" className="board-led" /><rect x="162" y="281" width="26" height="12" rx="2" fill="#a4b292" />
      <g fill="#132f22" stroke="#759a76" strokeWidth="2">{[[151,75],[369,75],[151,325],[369,325]].map(([x,y]) => <circle key={`${x}-${y}`} cx={x} cy={y} r="5" />)}</g>
    </g>
    {!compact && <g fill="currentColor" fontFamily="monospace" fontSize="10" opacity=".6"><text x="30" y="105">CAMERA MODULE</text><path d="M32 116h52l34 34" fill="none" stroke="currentColor" /><text x="363" y="334">CONNECTED HARDWARE</text></g>}
  </svg>;
}
