// node reashow.mjs <labelSubstring> [maxChars]  -> prints the REA result payload(s)
import fs from 'node:fs';
const dir = 'D:/Projects/MK9/MK9Rollback/analysis/rea-pass1/out';
const [sub, max] = process.argv.slice(2);
for (const f of fs.readdirSync(dir).filter(f => f.includes(sub)).sort()) {
  const j = JSON.parse(fs.readFileSync(`${dir}/${f}`, 'utf8'));
  const t = j.response?.content?.find(c => c.type === 'text')?.text ?? '';
  let o; try { o = JSON.parse(t); } catch { o = t; }
  const v = o?.raw_result?.value ?? o?.normalized_result ?? o?.result ?? o;
  let s = typeof v === 'string' ? v : JSON.stringify(v, null, 1);
  if (typeof v === 'object' && v && typeof v.pseudo_code === 'string') s = v.pseudo_code;
  if (typeof v === 'object' && v && typeof v.code === 'string') s = v.code;
  console.log(`######## ${f} isError=${j.response?.isError ?? false} ms=${j.ms} evidence=${o?.evidence_id ?? ''}`);
  if (o?.error) console.log('ERROR', JSON.stringify(o.error).slice(0, 1500));
  console.log(s.slice(0, Number(max || 12000)));
}
