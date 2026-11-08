// List code locations in .text whose 32-bit operand falls in [lo, hi) (globals in a .data range),
// grouped by target. usage: node immrange.mjs <exe> <lo hex> <hi hex>
import fs from 'node:fs';

const [exe, loArg, hiArg] = process.argv.slice(2);
const lo = parseInt(loArg, 16), hi = parseInt(hiArg, 16);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const sh = pe + 24 + b.readUInt16LE(pe + 20);
const text = { va: b.readUInt32LE(sh + 12), raw: b.readUInt32LE(sh + 20), rs: b.readUInt32LE(sh + 16) };
const hits = new Map();
for (let i = text.raw; i < text.raw + text.rs - 4; i++) {
  const v = b.readUInt32LE(i);
  if (v < lo || v >= hi) continue;
  const va = 0x400000 + text.va + (i - text.raw);
  if (!hits.has(v)) hits.set(v, []);
  hits.get(v).push(va.toString(16));
}
for (const [v, sites] of [...hits].sort((a, c) => a[0] - c[0]))
  console.log(`${v.toString(16)}: ${sites.length} refs: ${sites.slice(0, 8).join(' ')}${sites.length > 8 ? ' ...' : ''}`);
