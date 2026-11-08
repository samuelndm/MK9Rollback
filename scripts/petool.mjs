// Minimal static helper over the MKKE analysis copy (read-only).
// node petool.mjs bytes <va> <n>        hex dump
// node petool.mjs calls <va>            E8/E9 rel32 sites targeting va
// node petool.mjs imm <value>           4-byte little-endian occurrences (abs refs / vtable slots / pushes)
// node petool.mjs str <text>            ascii + utf16le occurrences, then imm refs to each
// node petool.mjs sig "<hex ?? hex>"    count + list matches across all sections
import fs from 'node:fs';
import crypto from 'node:crypto';
const FILE = 'D:/Projects/MK9/analysis/artifacts/DiscContentPC/MKKE.exe';
const EXPECT = 'D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6';
const buf = fs.readFileSync(FILE);
const sha = crypto.createHash('sha256').update(buf).digest('hex').toUpperCase();
if (sha !== EXPECT) throw new Error('hash mismatch ' + sha);
const pe = buf.readUInt32LE(0x3c);
const nsec = buf.readUInt16LE(pe + 6);
const optSize = buf.readUInt16LE(pe + 20);
const imageBase = buf.readUInt32LE(pe + 24 + 28);
const secs = [];
for (let i = 0; i < nsec; i++) {
  const o = pe + 24 + optSize + i * 40;
  secs.push({ name: buf.toString('latin1', o, o + 8).replace(/\0.*$/, ''), vsize: buf.readUInt32LE(o + 8), va: buf.readUInt32LE(o + 12), rsize: buf.readUInt32LE(o + 16), raw: buf.readUInt32LE(o + 20), ch: buf.readUInt32LE(o + 36) });
}
export const va2off = (va) => { const r = va - imageBase; for (const s of secs) if (r >= s.va && r < s.va + s.rsize) return s.raw + (r - s.va); return -1; };
export const off2va = (off) => { for (const s of secs) if (off >= s.raw && off < s.raw + s.rsize) return imageBase + s.va + (off - s.raw); return -1; };
const hex = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
const text = secs.find(s => s.name === '.text');
const [cmd, a1, a2] = process.argv.slice(2);
const P = (s) => parseInt(s, 16);
if (cmd === 'secs') { console.log(hex(imageBase), secs.map(s => `${s.name} va=${hex(imageBase + s.va)} vsz=${s.vsize.toString(16)} raw=${s.raw.toString(16)} rsz=${s.rsize.toString(16)} ch=${s.ch.toString(16)}`).join('\n')); }
if (cmd === 'bytes') { const o = va2off(P(a1)); const n = Number(a2 || 64); for (let i = 0; i < n; i += 16) console.log(hex(P(a1) + i), [...buf.subarray(o + i, o + Math.min(n, i + 16))].map(b => b.toString(16).padStart(2, '0')).join(' ')); }
if (cmd === 'calls') {
  const t = P(a1); const out = [];
  for (let o = text.raw; o < text.raw + text.rsize - 5; o++) {
    const op = buf[o]; if (op !== 0xe8 && op !== 0xe9) continue;
    const va = off2va(o); if (((va + 5 + buf.readInt32LE(o + 1)) >>> 0) === t) out.push(`${hex(va)} ${op === 0xe8 ? 'call' : 'jmp'}`);
  }
  console.log(out.length + ' sites'); console.log(out.join('\n'));
}
const immScan = (v) => { const out = []; const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); let i = -1; while ((i = buf.indexOf(b, i + 1)) >= 0) { const va = off2va(i); const sec = secs.find(s => i >= s.raw && i < s.raw + s.rsize); out.push(`${hex(va)} [${sec?.name}] prev=${buf.subarray(Math.max(0, i - 3), i).toString('hex')}`); } return out; };
if (cmd === 'imm') { const r = immScan(P(a1)); console.log(r.length + ' hits'); console.log(r.slice(0, 200).join('\n')); }
if (cmd === 'str') {
  for (const enc of ['latin1', 'utf16le']) {
    const needle = Buffer.from(a1, enc); let i = -1;
    while ((i = buf.indexOf(needle, i + 1)) >= 0) {
      // walk back to string start
      let s = i; if (enc === 'latin1') { while (s > 0 && buf[s - 1] >= 0x20 && buf[s - 1] < 0x7f) s--; } else { while (s > 1 && buf[s - 2] >= 0x20 && buf[s - 2] < 0x7f && buf[s - 1] === 0) s -= 2; }
      let e = i; if (enc === 'latin1') { while (buf[e] >= 0x20 && buf[e] < 0x7f) e++; } else { while (buf[e] >= 0x20 && buf[e] < 0x7f && buf[e + 1] === 0) e += 2; }
      const va = off2va(s); const refs = immScan(va);
      console.log(`${enc} ${hex(va)} "${buf.toString(enc, s, e).slice(0, 120)}" refs=${refs.length}`); for (const r of refs.slice(0, 12)) console.log('   ' + r);
    }
  }
}
if (cmd === 'sig') {
  const toks = a1.trim().split(/\s+/); const pat = toks.map(t => t === '??' || t === '?' ? -1 : parseInt(t, 16));
  const hits = [];
  for (const s of secs) {
    if (!(s.ch & 0x20000000)) continue; // executable sections only
    outer: for (let o = s.raw; o <= s.raw + s.rsize - pat.length; o++) {
      for (let k = 0; k < pat.length; k++) if (pat[k] >= 0 && buf[o + k] !== pat[k]) continue outer;
      hits.push(hex(off2va(o)));
    }
  }
  console.log(JSON.stringify({ count: hits.length, hits: hits.slice(0, 20) }));
}
