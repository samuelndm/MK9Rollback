// Given a code address referenced from a vtable slot, find the vtable start and, if
// MSVC RTTI is present, the class name from the complete-object locator at vt[-1].
// usage: node vtclass.mjs <exe> <slotVA hex>
import fs from 'node:fs';

const [exe, slotArg] = process.argv.slice(2);
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
const rd = (v) => b.readUInt32LE(fo(v));
const text = secs[0];
const isCode = (v) => v >= 0x400000 + text.va && v < 0x400000 + text.va + text.vs;

const slot = parseInt(slotArg, 16);
let start = slot;
while (isCode(rd(start - 4))) start -= 4;
const col = rd(start - 4);
console.log(`vtable start ${start.toString(16)} slot index ${(slot - start) / 4} vt[-1] ${col.toString(16)}`);
if (col > 0x400000 && col < 0x1100000 && fo(col) >= 0) {
  const td = rd(col + 12);
  if (fo(td) >= 0) {
    const name = b.toString('latin1', fo(td + 8), fo(td + 8) + 120).split('\0')[0];
    console.log(`RTTI type descriptor ${td.toString(16)} name ${name}`);
  }
}

