// Dump N entries of a vtable in the PE image.
// usage: node vtdump.mjs <exe> <vtable hex> [count]
import fs from 'node:fs';

const [exe, vtArg, countArg] = process.argv.slice(2);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const n = b.readUInt16LE(pe + 6);
const sh = pe + 24 + b.readUInt16LE(pe + 20);
const secs = [];
for (let i = 0; i < n; i++) {
  const o = sh + i * 40;
  secs.push({ va: b.readUInt32LE(o + 12), vs: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20), rs: b.readUInt32LE(o + 16) });
}
const fo = (va) => {
  const r = va - 0x400000;
  for (const s of secs) if (r >= s.va && r < s.va + s.vs && r - s.va < s.rs) return s.raw + (r - s.va);
  return -1;
};
const vt = parseInt(vtArg, 16);
const count = Number(countArg ?? 16);
const out = [];
for (let i = 0; i < count; i++) out.push(`+0x${(i * 4).toString(16)}=${b.readUInt32LE(fo(vt + 4 * i)).toString(16)}`);
console.log(out.join(' '));
