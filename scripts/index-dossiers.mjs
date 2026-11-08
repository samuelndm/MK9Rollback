// Index all prior direct-Ghidra dossiers: entry -> {file, name, has pseudocode}.
// usage: node index-dossiers.mjs [dumpEntryHex ...]
import fs from 'node:fs';
import path from 'node:path';
const dir = 'D:/Projects/MK9/analysis';
const files = fs.readdirSync(dir).filter(f => /^direct-ghidra-focused-.*\.json$/.test(f));
const idx = new Map();
let sample;
for (const f of files) {
  const j = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
  for (const fn of j.functions ?? []) {
    sample ??= Object.keys(fn);
    const e = (fn.entry ?? fn.address ?? '').toString().toLowerCase().replace(/^0x/, '').padStart(8, '0');
    if (!idx.has(e)) idx.set(e, { files: [], name: fn.name, fn });
    idx.get(e).files.push(f);
  }
}
const want = process.argv.slice(2).map(s => s.toLowerCase().replace(/^0x/, '').padStart(8, '0'));
if (want.length === 0) {
  console.log('keys:', sample);
  console.log([...idx.keys()].sort().join(' '));
} else {
  for (const w of want) {
    const r = idx.get(w);
    if (!r) { console.log(`== ${w}: NOT IN DOSSIERS`); continue; }
    const fn = r.fn;
    console.log(`== ${w} ${fn.databaseFunctionName} files=${r.files.join(',')}`);
    console.log(`cc=${fn.callingConvention} proto=${fn.prototype} sig=${fn.signaturePrefixHex}`);
    console.log('callers:', JSON.stringify(fn.callers));
    console.log('callees:', JSON.stringify(fn.callees));
    if (process.env.ASM) console.log('asm:\n' + (Array.isArray(fn.assembly) ? fn.assembly.map(a => typeof a === 'string' ? a : `${a.address} ${a.text ?? a.instruction ?? JSON.stringify(a)}`).join('\n') : fn.assembly));
    console.log('pseudo:\n' + String(fn.pseudocode).slice(0, Number(process.env.MAXC || 20000)));
  }
}
