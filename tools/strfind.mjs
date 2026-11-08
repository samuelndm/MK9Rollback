// Find ASCII / UTF-16 strings matching a regex in the PE image, with their VAs, and the code
// locations that push or reference them (imm32 in .text).
// usage: node strfind.mjs <exe> <regex> [maxHits]
import fs from 'node:fs';

const [exe, reArg, maxArg] = process.argv.slice(2);
const re = new RegExp(reArg, 'i');
const max = parseInt(maxArg || '60', 10);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const n = b.readUInt16LE(pe + 6);
const sh = pe + 24 + b.readUInt16LE(pe + 20);
const secs = [];
for (let i = 0; i < n; i++) {
  const o = sh + i * 40;
  secs.push({ name: b.toString('latin1', o, o + 8).replace(/\0.*$/, ''), va: b.readUInt32LE(o + 12), vs: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20), rs: b.readUInt32LE(o + 16) });
}
const toVa = (off) => { for (const s of secs) if (off >= s.raw && off < s.raw + s.rs) return 0x400000 + s.va + (off - s.raw); return -1; };
const text = secs[0];
function refs(va) {
  const key = Buffer.alloc(4); key.writeUInt32LE(va);
  const out = [];
  for (let i = b.indexOf(key, text.raw); i >= 0 && i < text.raw + text.rs && out.length < 4; i = b.indexOf(key, i + 1)) out.push(toVa(i).toString(16));
  return out;
}
let hits = 0;
// ASCII
for (let i = 0; i < b.length && hits < max; ) {
  let j = i;
  while (j < b.length && b[j] >= 0x20 && b[j] < 0x7f) j++;
  if (j - i >= 5 && b[j] === 0) {
    const s = b.toString('latin1', i, j);
    if (re.test(s)) { const va = toVa(i); console.log(`A ${va.toString(16)} "${s}" refs: ${refs(va).join(' ')}`); hits++; }
  }
  i = j + 1;
}
// UTF-16
for (let i = 0; i + 1 < b.length && hits < max; i += 2) {
  let j = i, s = '';
  while (j + 1 < b.length && b[j + 1] === 0 && b[j] >= 0x20 && b[j] < 0x7f) { s += String.fromCharCode(b[j]); j += 2; }
  if (s.length >= 5 && re.test(s)) { const va = toVa(i); console.log(`W ${va.toString(16)} L"${s}" refs: ${refs(va).join(' ')}`); hits++; }
  i = Math.max(i, j);
}
