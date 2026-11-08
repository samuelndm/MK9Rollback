// node rea-queue.mjs <batchName> <tool> <argKey> <value...>   -> writes inbox/<batchName>.json
// or: node rea-queue.mjs <batchName> --json '<array>'
import fs from 'node:fs';
const [batch, tool, key, ...vals] = process.argv.slice(2);
const inbox = 'D:/Projects/MK9/MK9Rollback/analysis/rea-pass1/inbox';
let reqs;
if (tool === '--json') reqs = JSON.parse(key);
else if (tool === '--file') reqs = JSON.parse(fs.readFileSync(key, 'utf8').replace(/^﻿/, ''));
else reqs = vals.map(v => ({ label: `${batch}-${tool}-${v.replace(/[^A-Za-z0-9]/g, '')}`, name: tool, arguments: { [key]: v } }));
const tmp = `${inbox}/../${batch}.queued.tmp`;
fs.writeFileSync(tmp, JSON.stringify(reqs));
fs.renameSync(tmp, `${inbox}/${batch}.json`);
console.log(`queued ${reqs.length}`);
