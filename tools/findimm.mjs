// Find code locations that use a 32-bit immediate (e.g. a vtable address stored by a
// constructor), and print nearby pushed string literals (UE3 class registration names).
// usage: node findimm.mjs <exe> <imm hex>
import fs from 'node:fs';

const [exe, immArg] = process.argv.slice(2);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const n = b.readUInt16LE(pe + 6);
const sh = pe + 24 + b.readUInt16LE(pe + 20);
const secs = [];
for (let i = 0; i < n; i++) {
  const o = sh + i * 40;
  secs.push({ va: b.readUInt32LE(o + 12), vs: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20), rs: b.readUInt32LE(o + 16) });
}
const toVa = (off) => {
  for (const s of secs) if (off >= s.raw && off < s.raw + s.rs) return 0x400000 + s.va + (off - s.raw);
  return -1;
};
const fo = (va) => {
  const r = va - 0x400000;
  for (const s of secs) if (r >= s.va && r < s.va + s.vs && r - s.va < s.rs) return s.raw + (r - s.va);
  return -1;
};
const imm = parseInt(immArg, 16);
const key = Buffer.alloc(4);
key.writeUInt32LE(imm);
const text = secs[0];
let i = text.raw - 1;
while ((i = b.indexOf(key, i + 1)) >= 0 && i < text.raw + text.rs) {
  const va = toVa(i);
  // look for push imm32 (68 xx xx xx xx) of printable ASCII strings within +-0x200 bytes
  const names = new Set();
  for (let j = Math.max(text.raw, i - 0x300); j < i + 0x300; j++) {
    if (b[j] !== 0x68) continue;
    const p = b.readUInt32LE(j + 1);
    const f = fo(p);
    if (f < 0) continue;
    let s = '';
    for (let k = 0; k < 64; k++) {
      const c = b[f + k];
      if (c === 0) break;
      if (c < 0x20 || c > 0x7e) { s = ''; break; }
      s += String.fromCharCode(c);
    }
    if (s.length >= 4) names.add(s);
    // also UTF-16 strings
    let w = '';
    for (let k = 0; k < 64; k++) {
      const c = b.readUInt16LE(f + 2 * k);
      if (c === 0) break;
      if (c < 0x20 || c > 0x7e) { w = ''; break; }
      w += String.fromCharCode(c);
    }
    if (w.length >= 4) names.add('L"' + w + '"');
  }
  console.log(`use at ${va.toString(16)}: ${[...names].slice(0, 8).join(' | ')}`);
}
