// MKScript native table survey: find the contiguous run of code pointers in .data around a
// known native slot, then for every native list direct call targets (E8 rel32, one level deep
// into its first callee too) and absolute imm32/disp32 references, and flag the ones that reach
// given "marker" addresses (side-effect subsystems).
// usage: node nativetable.mjs <exe> <knownSlotVA hex> <marker hex>[=label] ...
import fs from 'node:fs';

const [exe, slotArg, ...markerArgs] = process.argv.slice(2);
const b = fs.readFileSync(exe);
const pe = b.readUInt32LE(0x3c);
const n = b.readUInt16LE(pe + 6);
const sh = pe + 24 + b.readUInt16LE(pe + 20);
const secs = [];
for (let i = 0; i < n; i++) {
  const o = sh + i * 40;
  secs.push({ name: b.toString('latin1', o, o + 8).replace(/\0.*$/, ''), va: b.readUInt32LE(o + 12), vs: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20), rs: b.readUInt32LE(o + 16) });
}
const fo = (va) => { const r = va - 0x400000; for (const s of secs) if (r >= s.va && r < s.va + s.rs) return s.raw + (r - s.va); return -1; };
const text = secs[0];
const isCode = (v) => v >= 0x400000 + text.va && v < 0x400000 + text.va + text.vs;
const rd = (va) => { const o = fo(va); return o < 0 ? 0 : b.readUInt32LE(o); };

// Table bounds.
const slot = parseInt(slotArg, 16);
// Unused ids are zero: tolerate runs of up to 16 zero slots inside the table.
const ok = (a) => isCode(rd(a));
const extend = (a, step) => {
  for (;;) {
    let k = 1;
    while (k <= 16 && rd(a + step * k) === 0) k++;
    if (k <= 16 && ok(a + step * k)) a += step * k;
    else return a;
  }
};
const lo = extend(slot, -4), hi = extend(slot, 4);
const count = (hi - lo) / 4 + 1;
console.log(`table ${lo.toString(16)}..${hi.toString(16)} (${count} natives)`);

const markers = markerArgs.map((m) => { const [a, l] = m.split('='); const [x, y] = a.split('-'); return { lo: parseInt(x, 16), hi: parseInt(y || x, 16), label: l || a }; });

// Shallow reference scan of a function: until the first ret at depth 0 or 0x180 bytes.
function scan(va, depth, seen, acc) {
  if (depth > 2 || seen.has(va)) return;
  seen.add(va);
  const o = fo(va);
  if (o < 0) return;
  for (let i = 0; i < 0x180; i++) {
    const op = b[o + i];
    if (op === 0xe8 || op === 0xe9) {
      const t = (va + i + 5 + b.readInt32LE(o + i + 1)) >>> 0;
      if (isCode(t)) { acc.add(t); if (op === 0xe8 || i < 0x10) scan(t, depth + 1, seen, acc); }
    }
    // imm32 / disp32 references to .data/.rdata globals
    const v = b.readUInt32LE(o + i);
    if (v >= 0x00c50000 && v < 0x01100000) acc.add(v);
    if (op === 0xc3 && i > 4) break;
  }
}
const rows = [];
for (let k = 0; k < count; k++) {
  const f = rd(lo + 4 * k);
  const acc = new Set();
  scan(f, 0, new Set(), acc);
  const hits = [];
  for (const m of markers) for (const r of acc) if (r >= m.lo && r <= m.hi) { hits.push(m.label); break; }
  if (hits.length) rows.push(`#${k} slot ${(lo + 4 * k).toString(16)} native ${f.toString(16)}: ${[...new Set(hits)].join(', ')}`);
}
console.log(rows.join('\n'));
console.log(`${rows.length} natives touch a marker`);
