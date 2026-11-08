// node showdump.mjs <dump.txt> [entryHex ...] [--asm]  -> prints header + C (and asm if --asm)
import fs from 'node:fs';
const [file, ...rest] = process.argv.slice(2);
const asm = rest.includes('--asm');
const want = rest.filter(r => r !== '--asm').map(s => s.toLowerCase().replace(/^0x/, '').padStart(8, '0'));
const blocks = fs.readFileSync(file, 'utf8').split('\n================================================================\n');
for (const b of blocks.slice(1)) {
  const m = b.match(/ENTRY (\w+)/);
  if (!m) { console.log(b.slice(0, 300)); continue; }
  if (want.length && !want.includes(m[1])) continue;
  const [head, rest2] = b.split('\nC:\n');
  console.log('########', head.split('\n').filter(l => !l.startsWith('ASM')).join('\n'));
  if (rest2) { const [c, a] = rest2.split('\nASM:\n'); console.log(c.replace(/\n\s*\n/g, '\n')); if (asm) console.log(a); }
  else if (asm) console.log(b.split('\nASM:\n')[1]);
}
