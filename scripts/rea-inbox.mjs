// REA MCP driver (stdio). Spawns `rea mcp` with workspace-local Ghidra/JDK and a
// raised Ghidra heap, then processes request files dropped into an inbox dir.
// Each request file: {"label":"x","name":"tool","arguments":{...}} or [{...},...]
// Drop a file named CLOSE to end the session. Results -> outDir/<label>.json.
// Never touches the game install; targets the workspace analysis copy only.
import fs from 'node:fs/promises';
import path from 'node:path';
import { Client } from '../../tools/rea/node_modules/@modelcontextprotocol/client/dist/index.mjs';
import { StdioClientTransport } from '../../tools/rea/node_modules/@modelcontextprotocol/client/dist/stdio.mjs';

const mk9 = 'D:/Projects/MK9';
const base = path.join(mk9, 'MK9Rollback/analysis/rea-pass1');
const inbox = path.join(base, 'inbox');
const outDir = path.join(base, 'out');
await fs.mkdir(inbox, { recursive: true });
await fs.mkdir(outDir, { recursive: true });
const heap = process.env.MK9_REA_HEAP || '8G';
const logFile = await fs.open(path.join(base, 'rea-mcp-stderr.log'), 'a');
const statusPath = path.join(base, 'status.jsonl');
const status = async (o) => fs.appendFile(statusPath, JSON.stringify({ t: new Date().toISOString(), ...o }) + '\n');

const env = {
  ...process.env,
  GHIDRA_INSTALL_DIR: path.join(mk9, 'tools/ghidra_12.1.4_PUBLIC'),
  JAVA_HOME: path.join(mk9, 'tools/jdk-21.0.12.1+1'),
  REA_ANALYSIS_PROVIDER: 'ghidra',
  GHIDRA_HEADLESS_MAXMEM: heap,
  REA_GHIDRA_LANGUAGE_ID: 'x86:LE:32:default',
  REA_GHIDRA_COMPILER_SPEC_ID: 'windows',
  REA_GHIDRA_STARTUP_TIMEOUT_MS: process.env.MK9_REA_STARTUP_MS || '5400000',
};
const transport = new StdioClientTransport({ command: process.execPath,
  args: [path.join(mk9, 'tools/rea/node_modules/rea-agents/scripts/rea.mjs'), 'mcp'], cwd: mk9, env, stderr: 'pipe' });
transport.stderr.on('data', d => { void logFile.write(d).catch(() => {}); });
const client = new Client({ name: 'mk9rollback-pass1', version: '0.1.0' });
await client.connect(transport);
const catalog = await client.listTools();
await fs.writeFile(path.join(base, 'tool-names.json'), JSON.stringify(catalog.tools.map(t => t.name), null, 1));
await status({ ready: true, tools: catalog.tools.length, heap });

let running = true;
while (running) {
  const files = (await fs.readdir(inbox)).sort();
  if (files.length === 0) { await new Promise(r => setTimeout(r, 2000)); continue; }
  for (const f of files) {
    const p = path.join(inbox, f);
    if (f === 'CLOSE') { await fs.rm(p); running = false; break; }
    let reqs;
    try { reqs = JSON.parse(await fs.readFile(p, 'utf8')); } catch (e) { await status({ bad: f, e: String(e) }); await fs.rm(p); continue; }
    await fs.rm(p);
    for (const req of (Array.isArray(reqs) ? reqs : [reqs])) {
      const t0 = Date.now();
      await status({ start: req.label, name: req.name });
      try {
        const res = await client.callTool({ name: req.name, arguments: req.arguments ?? {} }, { timeout: 6000000, resetTimeoutOnProgress: true });
        await fs.writeFile(path.join(outDir, `${req.label}.json`), JSON.stringify({ request: req, ms: Date.now() - t0, response: res }, null, 1));
        await status({ done: req.label, ms: Date.now() - t0, isError: res.isError ?? false });
      } catch (e) {
        await status({ fail: req.label, ms: Date.now() - t0, e: String(e) });
      }
    }
  }
}
await client.close();
await logFile.close();
await status({ closed: true });
