// Name the imports behind IAT slots (call dword ptr [slot]).
// usage: node iat.mjs <exe> <slot hex> [<slot hex> ...]
import fs from 'node:fs';

const [exe, ...slots] = process.argv.slice(2);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const n = b.readUInt16LE(pe + 6);
const opt = pe + 24;
const sh = opt + b.readUInt16LE(pe + 20);
const secs = [];
for (let i = 0; i < n; i++) {
  const o = sh + i * 40;
  secs.push({ va: b.readUInt32LE(o + 12), vs: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20) });
}
const fo = (rva) => {
  for (const s of secs) if (rva >= s.va && rva < s.va + Math.max(s.vs, 1)) return s.raw + (rva - s.va);
  return -1;
};
const str = (rva) => { const o = fo(rva); let s = ''; for (let k = o; b[k]; k++) s += String.fromCharCode(b[k]); return s; };
const impRva = b.readUInt32LE(opt + 96 + 8);
const names = new Map();
for (let d = fo(impRva); b.readUInt32LE(d + 12); d += 20) {
  const dll = str(b.readUInt32LE(d + 12));
  const ilt = b.readUInt32LE(d) || b.readUInt32LE(d + 16);
  const iat = b.readUInt32LE(d + 16);
  for (let i = 0; ; i++) {
    const e = b.readUInt32LE(fo(ilt) + 4 * i);
    if (!e) break;
    const name = e & 0x80000000 ? `#${e & 0xffff}` : str(e + 2);
    names.set(0x400000 + iat + 4 * i, `${dll}!${name}`);
  }
}
for (const s of slots) { const v = parseInt(s, 16); console.log(`${v.toString(16)}: ${names.get(v) ?? '?'}`); }
