// Infer calling convention from REA procedure_assembly outputs (r30-*):
// RET n (callee cleanup), first use of ECX/EDX (read-before-write => register arg), [EBP+8..] stack arg use.
import fs from 'node:fs';
const dir = 'D:/Projects/MK9/MK9Rollback/analysis/rea-pass1/out';
const rows = [];
for (const f of fs.readdirSync(dir).filter(f => f.startsWith(process.argv[2] || 'r30-procedure_assembly'))) {
  const j = JSON.parse(fs.readFileSync(`${dir}/${f}`, 'utf8'));
  const t = j.response.content[0].text; let o; try { o = JSON.parse(t); } catch { o = t; }
  const asm = typeof o === 'string' ? o : (o.raw_result?.value ?? JSON.stringify(o));
  const lines = String(asm).split('\n').filter(Boolean);
  const rets = [...new Set(lines.filter(l => /: RET/.test(l)).map(l => l.split(': ')[1].trim()))];
  const regFirst = (r) => { // read-before-write heuristic over linear listing (first 40 insns)
    for (const l of lines.slice(0, 40)) {
      const ins = l.split(': ')[1] ?? ''; const [op, ...rest] = ins.split(' '); const ops = rest.join(' ');
      const [dst, src] = ops.split(/,(.*)/s);
      const re = new RegExp(`\\b${r}\\b`);
      if (/^CALL/.test(op)) return 'clobbered-by-call';
      if (src !== undefined && re.test(src)) return 'read';
      if (dst && re.test(dst) && /\[/.test(dst)) return 'read';
      if (dst && re.test(dst) && /^(MOV|LEA|XOR|POP|MOVZX|MOVSX)$/.test(op)) { if (op === 'XOR' && /ECX, ECX|EDX, EDX/.test(ops)) return 'written'; if (op !== 'XOR') return 'written'; }
      if (dst && re.test(dst)) return 'read';
      if (/^PUSH$/.test(op) && re.test(ops)) return 'read';
    }
    return 'unused-early';
  };
  const stackArgs = [...new Set(lines.flatMap(l => [...l.matchAll(/\[EBP \+ (0x[0-9a-f]+)\]/g)].map(m => m[1])))].filter(x => parseInt(x, 16) >= 8).sort();
  rows.push({ fn: f.replace(/^.*-(0x[0-9a-f]+)\.json$/, '$1'), insns: lines.length, rets, ecx: regFirst('ECX'), edx: regFirst('EDX'), ebpArgs: stackArgs, head: lines.slice(0, 4).map(l => l.split(': ')[1]).join(' ; ') });
}
console.log(JSON.stringify(rows, null, 1));
