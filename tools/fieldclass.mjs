// Classify differing dwords between two trials across several aligned frames.
// usage: node fieldclass.mjs <dumpsDir> <stamp> <trialA> <trialB> <frame,frame,...>
// For each region offset that differs at any frame:
//   const-delta : b-a identical at every frame (shifted counter / absolute stamp)
//   const-pair  : same (a,b) pair at every frame (static per-trial value, e.g. pointer/old history)
//   converging  : differs at early frames, equal later
//   VARYING     : delta changes over time -> genuine trajectory divergence candidate
import fs from 'node:fs';
import path from 'node:path';

const [dir, stamp, ta, tb, framesArg] = process.argv.slice(2);
const frames = framesArg.split(',').map(Number);
function load(label) {
  const idx = fs.readFileSync(path.join(dir, `${label}.txt`), 'utf8').trim().split(/\r?\n/);
  const regions = idx.slice(1).map(l => {
    const m = /^(\S+) base=(\w+) size=(\d+) fileoff=(\d+)/.exec(l);
    return { name: m[1], size: Number(m[3]), off: Number(m[4]) };
  });
  return { regions, bin: fs.readFileSync(path.join(dir, `${label}.bin`)), gf: Number(/gf=(\d+)/.exec(idx[0])[1]) };
}
const A = frames.map(f => load(`t${stamp}-${ta}-f${f}`));
const B = frames.map(f => load(`t${stamp}-${tb}-f${f}`));
const f32 = new DataView(new ArrayBuffer(4));
const fl = u => { f32.setUint32(0, u, true); return f32.getFloat32(0, true); };
const out = {};
for (const reg of A[0].regions) {
  const res = { 'rel-stamp': [], 'const-delta': [], 'const-pair': [], converging: [], VARYING: [] };
  const size = Math.min(...A.map(d => d.regions.find(r => r.name === reg.name).size),
                        ...B.map(d => d.regions.find(r => r.name === reg.name).size));
  for (let o = 0; o + 4 <= size; o += 4) {
    const vals = frames.map((_, i) => {
      const ra = A[i].regions.find(r => r.name === reg.name), rb = B[i].regions.find(r => r.name === reg.name);
      return [A[i].bin.readUInt32LE(ra.off + o), B[i].bin.readUInt32LE(rb.off + o)];
    });
    if (vals.every(([a, b]) => a === b)) continue;
    const deltas = vals.map(([a, b]) => (b - a) >>> 0);
    // relative-stamp: value == (GameFrame at dump - k) in both trials (int or float encoding)
    const relInt = vals.every(([a, b], i) => (A[i].gf - a) === (B[i].gf - b));
    const relFlt = vals.every(([a, b], i) => Math.abs((A[i].gf - fl(a)) - (B[i].gf - fl(b))) < 0.75 && fl(a) > 1000);
    let kind;
    if (relInt || relFlt) kind = 'rel-stamp';
    else if (vals.every(([a, b]) => a === vals[0][0] && b === vals[0][1])) kind = 'const-pair';
    else if (deltas.every(d => d === deltas[0])) kind = 'const-delta';
    else if (vals[vals.length - 1][0] === vals[vals.length - 1][1]) kind = 'converging';
    else kind = 'VARYING';
    res[kind].push(`+0x${o.toString(16)} ` + vals.map(([a, b]) => `${a.toString(16)}/${b.toString(16)}`).join(' ') +
      (kind === 'VARYING' ? '  f:' + vals.map(([a, b]) => `${fl(a).toPrecision(5)}/${fl(b).toPrecision(5)}`).join(' ') : ''));
  }
  out[reg.name] = res;
}
console.log(`frames ${frames.join(',')}  gfA=${A.map(d => d.gf)} gfB=${B.map(d => d.gf)}`);
for (const [name, res] of Object.entries(out)) {
  const counts = Object.fromEntries(Object.entries(res).map(([k, v]) => [k, v.length]));
  console.log(`== ${name} ${JSON.stringify(counts)}`);
  for (const line of res.VARYING.slice(0, 30)) console.log('   VARYING ' + line);
  for (const line of res.converging.slice(0, 8)) console.log('   converging ' + line);
}
