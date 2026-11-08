// Cross references to a VA: absolute dwords anywhere in the image (vtables, pushes) and
// E8/E9 rel32 calls/jumps in .text.
// usage: node xref.mjs <exe> <va hex> [<va hex> ...]
import fs from 'node:fs';

const [exe, ...vas] = process.argv.slice(2);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const n = b.readUInt16LE(pe + 6);
const sh = pe + 24 + b.readUInt16LE(pe + 20);
const secs = [];
for (let i = 0; i < n; i++) {
  const o = sh + i * 40;
  secs.push({ name: b.toString('latin1', o, o + 8).replace(/\0.*$/, ''), va: b.readUInt32LE(o + 12), vs: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20), rs: b.readUInt32LE(o + 16) });
}
const toVa = (off) => {
  for (const s of secs) if (off >= s.raw && off < s.raw + s.rs) return [0x400000 + s.va + (off - s.raw), s.name];
  return [-1, '?'];
};
const text = secs[0];
for (const a of vas) {
  const t = parseInt(a, 16);
  const key = Buffer.alloc(4);
  key.writeUInt32LE(t);
  for (let i = b.indexOf(key); i >= 0; i = b.indexOf(key, i + 1)) {
    const [va, sec] = toVa(i);
    console.log(`${t.toString(16)}: abs at ${va.toString(16)} (${sec})`);
  }
  for (let i = text.raw; i < text.raw + text.rs - 5; i++) {
    if (b[i] !== 0xe8 && b[i] !== 0xe9) continue;
    const [va] = toVa(i);
    if ((va + 5 + b.readInt32LE(i + 1)) >>> 0 === t) console.log(`${t.toString(16)}: ${b[i] === 0xe8 ? 'call' : 'jmp'} at ${va.toString(16)}`);
  }
}
