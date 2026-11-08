// Print the ASCII and UTF-16 string at each VA. usage: node cstr.mjs <exe> <va hex> ...
import fs from 'node:fs';

const [exe, ...vas] = process.argv.slice(2);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const n = b.readUInt16LE(pe + 6);
const sh = pe + 24 + b.readUInt16LE(pe + 20);
const secs = [];
for (let i = 0; i < n; i++) {
  const o = sh + i * 40;
  secs.push({ va: b.readUInt32LE(o + 12), raw: b.readUInt32LE(o + 20), rs: b.readUInt32LE(o + 16) });
}
const fo = (v) => { const r = v - 0x400000; for (const x of secs) if (r >= x.va && r < x.va + x.rs) return x.raw + (r - x.va); return -1; };
for (const s of vas) {
  const o = fo(parseInt(s, 16));
  if (o < 0) { console.log(`${s}: not in file`); continue; }
  let a = '', w = '';
  for (let k = 0; k < 120 && b[o + k]; k++) a += String.fromCharCode(b[o + k]);
  for (let k = 0; k < 120 && b.readUInt16LE(o + 2 * k); k++) w += String.fromCharCode(b.readUInt16LE(o + 2 * k));
  console.log(`${s}: ascii="${a}" utf16="${w}"`);
}
