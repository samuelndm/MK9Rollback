// Build unique, relocation-safe byte signatures for candidate entries.
// Wildcards: every byte covered by a .reloc HIGHLOW entry (absolute VA), and the rel32 of
// direct E8 call / E9 jmp instructions (instruction starts taken from REA procedure_assembly).
// Grows from the function entry until the pattern is unique across executable sections.
// node sigmaker.mjs 0xVA [0xVA...]  -> JSON lines
import fs from 'node:fs';
import crypto from 'node:crypto';
const FILE = 'D:/Projects/MK9/analysis/artifacts/DiscContentPC/MKKE.exe';
const buf = fs.readFileSync(FILE);
if (crypto.createHash('sha256').update(buf).digest('hex').toUpperCase() !== 'D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6') throw new Error('hash');
const pe = buf.readUInt32LE(0x3c), nsec = buf.readUInt16LE(pe + 6), optSize = buf.readUInt16LE(pe + 20), opt = pe + 24;
const imageBase = buf.readUInt32LE(opt + 28);
const secs = []; for (let i = 0; i < nsec; i++) { const o = opt + optSize + i * 40; secs.push({ name: buf.toString('latin1', o, o + 8).replace(/\0.*$/, ''), va: buf.readUInt32LE(o + 12), rsize: buf.readUInt32LE(o + 16), raw: buf.readUInt32LE(o + 20), ch: buf.readUInt32LE(o + 36) }); }
const rva2off = r => { for (const s of secs) if (r >= s.va && r < s.va + s.rsize) return s.raw + r - s.va; return -1; };
// relocations
const relocDir = opt + 96 + 5 * 8; const relRva = buf.readUInt32LE(relocDir), relSize = buf.readUInt32LE(relocDir + 4);
const relocated = new Set();
for (let p = rva2off(relRva), end = p + relSize; p < end;) { const page = buf.readUInt32LE(p), size = buf.readUInt32LE(p + 4); if (!size) break; for (let k = 8; k < size; k += 2) { const e = buf.readUInt16LE(p + k); if ((e >> 12) === 3) relocated.add(page + (e & 0xfff)); } p += size; }
// instruction starts from REA assembly
const outDir = 'D:/Projects/MK9/MK9Rollback/analysis/rea-pass1/out';
const asmFor = (va) => { const f = fs.readdirSync(outDir).find(f => f.includes('procedure_assembly') && f.endsWith(`-0x${va.toString(16).padStart(8, '0')}.json`)); if (!f) return null; const j = JSON.parse(fs.readFileSync(`${outDir}/${f}`, 'utf8')); let t = j.response.content[0].text; try { const o = JSON.parse(t); t = o.raw_result?.value ?? t; } catch {} if (typeof t !== 'string') return null; return t.split('\n').map(l => { const m = l.match(/^0x([0-9a-f]+): (.*)$/); return m ? { va: parseInt(m[1], 16), text: m[2] } : null; }).filter(Boolean); };
const exec = secs.filter(s => s.ch & 0x20000000);
const count = (pat) => { let n = 0, first = -1; for (const s of exec) { outer: for (let o = s.raw; o <= s.raw + s.rsize - pat.length; o++) { for (let k = 0; k < pat.length; k++) if (pat[k] >= 0 && buf[o + k] !== pat[k]) continue outer; n++; if (first < 0) first = imageBase + s.va + (o - s.raw); if (n > 1) return { n, first }; } } return { n, first }; };
for (const arg of process.argv.slice(2)) {
  const va = parseInt(arg, 16); const rva = va - imageBase; const off = rva2off(rva);
  const asm = asmFor(va); const wild = new Set();
  for (let i = 0; i < 256; i++) if (relocated.has(rva + i)) for (let k = 0; k < 4; k++) wild.add(i + k);
  if (asm) for (const ins of asm) { const i = ins.va - va; if (i < 0 || i > 255) continue; const op = buf[off + i]; if ((op === 0xe8 || op === 0xe9) && /^(CALL|JMP) 0x/.test(ins.text)) for (let k = 1; k <= 4; k++) wild.add(i + k); }
  else for (let i = 0; i < 256; i++) { const op = buf[off + i]; if (op === 0xe8) { const t = (va + i + 5 + buf.readInt32LE(off + i + 1)) >>> 0; if (t >= 0x401000 && t < 0xc55000) for (let k = 1; k <= 4; k++) wild.add(i + k); } }
  // stop at instruction boundary when available
  const bounds = asm ? asm.map(x => x.va - va).filter(x => x > 0 && x <= 255) : [...Array(256).keys()];
  let result = null;
  for (const len of bounds) {
    if (len < 12) continue;
    const pat = [...buf.subarray(off, off + len)].map((b, i) => wild.has(i) ? -1 : b);
    const fixed = pat.filter(b => b >= 0).length;
    if (fixed < 10) continue;
    const c = count(pat);
    if (c.n === 1 && c.first === va) { result = { pat, len, fixed }; break; }
  }
  const hex = result ? result.pat.map(b => b < 0 ? '??' : b.toString(16).padStart(2, '0').toUpperCase()).join(' ') : null;
  console.log(JSON.stringify({ va: '0x' + va.toString(16).toUpperCase().padStart(8, '0'), rva: '0x' + rva.toString(16).toUpperCase(), asmBoundaries: !!asm, signature: hex, length: result?.len ?? null, fixedBytes: result?.fixed ?? null, unique: !!result }));
}
