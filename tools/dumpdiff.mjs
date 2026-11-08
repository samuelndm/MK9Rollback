// Byte-level diff of two StateHash dumps (dumps/<label>.bin + .txt).
// usage: node dumpdiff.mjs <dumpsDir> <labelA> <labelB> [maxPerRegion]
// Each differing dword is classified:
//   frame-stamp : b - a == gfB - gfA (absolute GameFrame stored in state)
//   pointer     : both values look like heap/module addresses
//   float       : both decode to finite floats of moderate magnitude
//   other
import fs from 'node:fs';
import path from 'node:path';

const [dir, la, lb, maxArg] = process.argv.slice(2);
const max = Number(maxArg ?? 40);
function load(label) {
  const idx = fs.readFileSync(path.join(dir, `${label}.txt`), 'utf8').trim().split(/\r?\n/);
  const gf = Number(/gf=(\d+)/.exec(idx[0])[1]);
  const regions = idx.slice(1).map(l => {
    const m = /^(\S+) base=(\w+) size=(\d+) fileoff=(\d+)/.exec(l);
    return { name: m[1], base: parseInt(m[2], 16), size: Number(m[3]), off: Number(m[4]) };
  });
  return { gf, regions, bin: fs.readFileSync(path.join(dir, `${label}.bin`)) };
}
const A = load(la), B = load(lb);
const dgf = (B.gf - A.gf) >>> 0;
console.log(`A=${la} gf=${A.gf}  B=${lb} gf=${B.gf}  dGF=${B.gf - A.gf}`);
const f32 = new DataView(new ArrayBuffer(4));
const asFloat = u => { f32.setUint32(0, u, true); return f32.getFloat32(0, true); };
const looksPtr = u => u >= 0x00400000 && u < 0x80000000 && (u & 3) === 0;
const looksFloat = u => { const f = asFloat(u); return Number.isFinite(f) && f !== 0 && Math.abs(f) > 1e-6 && Math.abs(f) < 1e7; };

const summary = [];
for (const ra of A.regions) {
  const rb = B.regions.find(r => r.name === ra.name);
  if (!rb || !ra.size || !rb.size) { summary.push(`${ra.name}: missing/unreadable`); continue; }
  const size = Math.min(ra.size, rb.size);
  const counts = { 'frame-stamp': 0, pointer: 0, float: 0, other: 0 };
  const rows = [];
  for (let o = 0; o + 4 <= size; o += 4) {
    const a = A.bin.readUInt32LE(ra.off + o), b = B.bin.readUInt32LE(rb.off + o);
    if (a === b) continue;
    let kind = 'other';
    if (((b - a) >>> 0) === dgf) kind = 'frame-stamp';
    else if (looksPtr(a) && looksPtr(b)) kind = 'pointer';
    else if (looksFloat(a) && looksFloat(b)) kind = 'float';
    counts[kind]++;
    if (rows.length < max) rows.push(`  +0x${o.toString(16).padStart(4, '0')}  ${a.toString(16).padStart(8, '0')} -> ${b.toString(16).padStart(8, '0')}  ${kind}${kind === 'float' ? ` (${asFloat(a).toFixed(4)} -> ${asFloat(b).toFixed(4)})` : ''}`);
  }
  const total = Object.values(counts).reduce((x, y) => x + y, 0);
  summary.push(`${ra.name} base ${ra.base.toString(16)}/${rb.base.toString(16)} size ${size}: ${total} differing dwords ${JSON.stringify(counts)}`);
  if (total) { console.log(`== ${ra.name}`); console.log(rows.join('\n')); }
}
console.log('\nSUMMARY'); console.log(summary.join('\n'));
