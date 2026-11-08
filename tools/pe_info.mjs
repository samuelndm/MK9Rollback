// pe_info.mjs - read-only, execution-free PE identity extractor (PE32 / PE32+).
// Usage: node pe_info.mjs <exe> [--acf <appmanifest.acf>] [--out <file.json>]
// Opens the target read-only and never writes next to it. Every field is "measured" from file bytes
// unless the key is under `inferred` / noted as heuristic.
import fs from 'node:fs';
import crypto from 'node:crypto';

const argv = process.argv.slice(2);
const exePath = argv.find(a => !a.startsWith('--') && argv[argv.indexOf(a) - 1] !== '--acf' && argv[argv.indexOf(a) - 1] !== '--out');
const opt = n => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : null; };
if (!exePath) { console.error('usage: node pe_info.mjs <exe> [--acf file] [--out file.json]'); process.exit(2); }

const b = fs.readFileSync(exePath);            // read-only
const hex = (n, w = 0) => '0x' + Number(n).toString(16).toUpperCase().padStart(w, '0');
const need = (o, n) => { if (!Number.isSafeInteger(o) || o < 0 || o + n > b.length) throw new Error(`OOB ${o}+${n}`); };
const u8 = o => { need(o, 1); return b[o]; };
const u16 = o => { need(o, 2); return b.readUInt16LE(o); };
const u32 = o => { need(o, 4); return b.readUInt32LE(o); };
const cstr = (o, max = 4096) => { need(o, 1); let e = o; while (e < b.length && b[e] && e - o < max) e++; return b.toString('latin1', o, e); };
const flags = (v, table) => table.filter(([m]) => (v & m) >>> 0 === m >>> 0 && m !== 0).map(([, n]) => n);

// ---------- hashes ----------
const hashes = {};
for (const h of ['sha256', 'sha1', 'md5']) hashes[h] = crypto.createHash(h).update(b).digest('hex').toUpperCase();

// ---------- headers ----------
if (u16(0) !== 0x5a4d) throw new Error('no MZ');
const peOff = u32(0x3c);
if (u32(peOff) !== 0x4550) throw new Error('no PE sig');
const machine = u16(peOff + 4), nSec = u16(peOff + 6), tds = u32(peOff + 8), symPtr = u32(peOff + 12), nSyms = u32(peOff + 16),
  optSize = u16(peOff + 20), chars = u16(peOff + 22);
const o = peOff + 24, magic = u16(o), is64 = magic === 0x20b;
const machineNames = { 0x14c: 'I386', 0x8664: 'AMD64', 0x1c0: 'ARM', 0xaa64: 'ARM64' };
const charsNames = [[1, 'RELOCS_STRIPPED'], [2, 'EXECUTABLE_IMAGE'], [4, 'LINE_NUMS_STRIPPED'], [8, 'LOCAL_SYMS_STRIPPED'], [0x20, 'LARGE_ADDRESS_AWARE'], [0x100, '32BIT_MACHINE'], [0x200, 'DEBUG_STRIPPED'], [0x1000, 'SYSTEM'], [0x2000, 'DLL']];
const dllCharsNames = [[0x20, 'HIGH_ENTROPY_VA'], [0x40, 'DYNAMIC_BASE'], [0x80, 'FORCE_INTEGRITY'], [0x100, 'NX_COMPAT'], [0x200, 'NO_ISOLATION'], [0x400, 'NO_SEH'], [0x800, 'NO_BIND'], [0x1000, 'APPCONTAINER'], [0x2000, 'WDM_DRIVER'], [0x4000, 'GUARD_CF'], [0x8000, 'TERMINAL_SERVER_AWARE']];
const subsysNames = { 1: 'NATIVE', 2: 'WINDOWS_GUI', 3: 'WINDOWS_CUI', 5: 'OS2_CUI', 7: 'POSIX_CUI', 9: 'WINDOWS_CE_GUI', 10: 'EFI_APPLICATION', 14: 'XBOX' };
const imageBase = is64 ? Number(b.readBigUInt64LE(o + 24)) : u32(o + 28);
const dllChars = u16(o + 70);
const optional = {
  magic: hex(magic, 4), format: is64 ? 'PE32+' : 'PE32',
  linkerVersion: `${u8(o + 2)}.${u8(o + 3)}`,
  sizeOfCode: u32(o + 4), sizeOfInitializedData: u32(o + 8), sizeOfUninitializedData: u32(o + 12),
  entryPointRva: hex(u32(o + 16), 8), baseOfCode: hex(u32(o + 20), 8), ...(is64 ? {} : { baseOfData: hex(u32(o + 24), 8) }),
  imageBase: hex(imageBase, 8), sectionAlignment: hex(u32(o + 32)), fileAlignment: hex(u32(o + 36)),
  osVersion: `${u16(o + 40)}.${u16(o + 42)}`, imageVersion: `${u16(o + 44)}.${u16(o + 46)}`, subsystemVersion: `${u16(o + 48)}.${u16(o + 50)}`,
  win32VersionValue: u32(o + 52), sizeOfImage: hex(u32(o + 56), 8), sizeOfImageDec: u32(o + 56), sizeOfHeaders: hex(u32(o + 60)),
  checksumStored: hex(u32(o + 64), 8),
  subsystem: u16(o + 68), subsystemName: subsysNames[u16(o + 68)] || 'UNKNOWN',
  dllCharacteristics: hex(dllChars, 4), dllCharacteristicsFlags: flags(dllChars, dllCharsNames),
  stackReserve: hex(u32(o + 72)), stackCommit: hex(u32(o + 76)), heapReserve: hex(u32(o + 80)), heapCommit: hex(u32(o + 84)),
  numberOfRvaAndSizes: u32(o + 92),
};
const ddBase = o + (is64 ? 112 : 96);
const ddNames = ['EXPORT', 'IMPORT', 'RESOURCE', 'EXCEPTION', 'SECURITY(Authenticode)', 'BASERELOC', 'DEBUG', 'ARCHITECTURE', 'GLOBALPTR', 'TLS', 'LOAD_CONFIG', 'BOUND_IMPORT', 'IAT', 'DELAY_IMPORT', 'COM_DESCRIPTOR(CLR)', 'RESERVED'];
const dirs = Array.from({ length: Math.min(16, optional.numberOfRvaAndSizes) }, (_, i) => ({ index: i, name: ddNames[i], rva: hex(u32(ddBase + i * 8), 8), size: u32(ddBase + i * 8 + 4), present: !!(u32(ddBase + i * 8) || u32(ddBase + i * 8 + 4)) }));
const dir = i => ({ rva: u32(ddBase + i * 8), size: u32(ddBase + i * 8 + 4) });

const coff = {
  machine: hex(machine, 4), machineName: machineNames[machine] || 'UNKNOWN', numberOfSections: nSec,
  timeDateStamp: tds, timeDateStampUtc: new Date(tds * 1000).toISOString(),
  pointerToSymbolTable: symPtr, numberOfSymbols: nSyms, sizeOfOptionalHeader: optSize,
  characteristics: hex(chars, 4), characteristicsFlags: flags(chars, charsNames),
};

// ---------- sections ----------
const entropy = buf => { if (!buf.length) return 0; const c = new Array(256).fill(0); for (const x of buf) c[x]++; let e = 0; for (const n of c) if (n) { const p = n / buf.length; e -= p * Math.log2(p); } return +e.toFixed(4); };
const secCharNames = [[0x20, 'CNT_CODE'], [0x40, 'CNT_INITIALIZED_DATA'], [0x80, 'CNT_UNINITIALIZED_DATA'], [0x02000000, 'MEM_DISCARDABLE'], [0x04000000, 'MEM_NOT_CACHED'], [0x08000000, 'MEM_NOT_PAGED'], [0x10000000, 'MEM_SHARED'], [0x20000000, 'MEM_EXECUTE'], [0x40000000, 'MEM_READ'], [0x80000000, 'MEM_WRITE']];
const secTable = o + optSize;
const sections = Array.from({ length: nSec }, (_, i) => {
  const s = secTable + i * 40;
  const rec = { name: b.toString('latin1', s, s + 8).replace(/\0.*$/, ''), virtualSize: u32(s + 8), rva: u32(s + 12), rawSize: u32(s + 16), rawOffset: u32(s + 20), relocPtr: u32(s + 24), numReloc: u16(s + 32), characteristics: u32(s + 36) };
  rec.rawBytesInFile = rec.rawSize && rec.rawOffset + rec.rawSize <= b.length ? rec.rawSize : Math.max(0, Math.min(rec.rawSize, b.length - rec.rawOffset));
  rec.entropy = rec.rawSize ? entropy(b.subarray(rec.rawOffset, rec.rawOffset + rec.rawBytesInFile)) : 0;
  rec.characteristicsHex = hex(rec.characteristics, 8); rec.characteristicsFlags = flags(rec.characteristics, secCharNames);
  rec.endRva = rec.rva + Math.max(rec.virtualSize, rec.rawSize);
  return rec;
});
const headersSize = u32(o + 60);
const rvaToOff = (rva) => {
  if (rva < headersSize) return rva;
  for (const s of sections) if (rva >= s.rva && rva < s.rva + s.rawSize) return s.rawOffset + rva - s.rva;
  return null;
};
const R = rva => { const f = rvaToOff(rva); if (f === null) throw new Error(`unmapped RVA ${hex(rva)}`); return f; };
const secOfRva = rva => sections.find(s => rva >= s.rva && rva < s.rva + Math.max(s.virtualSize, s.rawSize));
const vaToRva = va => va - imageBase;
const overlayStart = Math.max(...sections.map(s => s.rawOffset + s.rawSize));

// ---------- PE checksum ----------
function computeChecksum() {
  const ckOff = o + 64; let sum = 0; const len = b.length;
  for (let i = 0; i + 1 < len; i += 2) { if (i === ckOff || i === ckOff + 2) continue; sum += b.readUInt16LE(i); sum = (sum & 0xffff) + (sum >>> 16); }
  if (len & 1) { sum += b[len - 1]; sum = (sum & 0xffff) + (sum >>> 16); }
  sum = (sum & 0xffff) + (sum >>> 16); return ((sum & 0xffff) + len) >>> 0;
}
const checksum = { stored: u32(o + 64), computed: computeChecksum() }; checksum.valid = checksum.stored === checksum.computed; checksum.storedHex = hex(checksum.stored, 8); checksum.computedHex = hex(checksum.computed, 8);

// ---------- Rich header ----------
function parseRich() {
  let richAt = -1;
  for (let i = 0x80; i + 8 <= peOff; i += 4) if (u32(i) === 0x68636952) { richAt = i; break; }
  if (richAt < 0) return { present: false };
  const key = u32(richAt + 4); let danS = -1;
  for (let i = richAt - 4; i >= 0x40; i -= 4) if ((u32(i) ^ key) >>> 0 === 0x536e6144) { danS = i; break; }
  if (danS < 0) return { present: false, note: 'Rich marker w/o DanS' };
  const entries = [];
  for (let i = danS + 16; i < richAt; i += 8) {
    const comp = (u32(i) ^ key) >>> 0, count = (u32(i + 4) ^ key) >>> 0;
    entries.push({ prodId: comp >>> 16, build: comp & 0xffff, count, compId: hex(comp, 8) });
  }
  const rol = (v, n) => ((v << (n & 31)) | (v >>> (32 - (n & 31)))) >>> 0;
  let cs = danS;
  for (let i = 0; i < danS; i++) { if (i >= 0x3c && i < 0x40) continue; cs = (cs + rol(b[i], i)) >>> 0; }
  for (const e of entries) cs = (cs + rol(parseInt(e.compId, 16) >>> 0, e.count)) >>> 0;
  return { present: true, offset: danS, key: hex(key, 8), checksumValid: cs === key, entries };
}
// Heuristic only: MSVC toolset by @comp.id build number
function guessToolset(build) {
  const t = [[[50727, 50727], 'VS2005 (8.0, 14.00.50727)'], [[21022, 21022], 'VS2008 RTM (9.0, 15.00.21022)'], [[30729, 30729], 'VS2008 SP1 (9.0, 15.00.30729)'],
  [[30319, 30319], 'VS2010 RTM (10.0, 16.00.30319)'], [[40219, 40219], 'VS2010 SP1 (10.0, 16.00.40219)'], [[4035, 4035], 'VC6 SP-ish'], [[2067, 2067], 'VS2002/2003-era'], [[3052, 3077], 'VS2003 (7.1)'], [[9044, 9782], 'VC6-era']];
  for (const [[lo, hi], n] of t) if (build >= lo && build <= hi) return n; return null;
}
const rich = parseRich();
if (rich.present) for (const e of rich.entries) e.toolsetGuess = guessToolset(e.build);

// ---------- imports ----------
const thunkW = is64 ? 8 : 4;
function readThunks(rva, iatRva, toRva = v => v) {
  const out = [];
  for (let i = 0; i < 20000; i++) {
    const off = R(rva + i * thunkW); const v = is64 ? b.readBigUInt64LE(off) : BigInt(u32(off));
    if (!v) return out;
    const isOrd = (v & (is64 ? 0x8000000000000000n : 0x80000000n)) !== 0n;
    if (isOrd) out.push({ ordinal: Number(v & 0xffffn), name: null, iatRva: hex(iatRva + i * thunkW) });
    else { const no = R(toRva(Number(v))); out.push({ ordinal: null, hint: u16(no), name: cstr(no + 2), iatRva: hex(iatRva + i * thunkW) }); }
  }
  throw new Error('unterminated thunks');
}
const imports = [];
{
  const d = dir(1);
  if (d.rva) for (let i = 0; ; i++) {
    const p = R(d.rva + i * 20); const f = [0, 1, 2, 3, 4].map(j => u32(p + j * 4));
    if (f.every(x => !x)) break;
    const name = cstr(R(f[3]));
    imports.push({ dll: name, kind: 'static', descriptorRva: hex(d.rva + i * 20), timeDateStamp: f[1], forwarderChain: f[2], firstThunk: hex(f[4]), originalFirstThunk: hex(f[0]), symbols: readThunks(f[0] || f[4], f[4]) });
  }
  const dd = dir(13);
  if (dd.rva) for (let i = 0; ; i++) {
    const p = R(dd.rva + i * 32); const f = Array.from({ length: 8 }, (_, j) => u32(p + j * 4));
    if (f.every(x => !x)) break;
    const rvaBased = f[0] & 1; const cv = v => rvaBased ? v : v - imageBase;
    imports.push({ dll: cstr(R(cv(f[1]))), kind: 'delay', attributes: hex(f[0]), descriptorRva: hex(dd.rva + i * 32), symbols: readThunks(cv(f[4] || f[3]), cv(f[3]), cv) });
  }
}
const boundImports = [];
{ // bound-import name offsets are relative to the directory start (file offset == RVA region is in headers)
  const d = dir(11);
  if (d.rva) { const base = R(d.rva); let p = base; for (let k = 0; k < 256; k++) { const ts = u32(p), no = u16(p + 4), nref = u16(p + 6); if (!ts && !no && !nref) break; boundImports.push({ dll: cstr(base + no), timeDateStamp: ts, timeDateStampUtc: new Date(ts * 1000).toISOString(), forwarderRefs: nref }); p += 8 * (1 + nref); } }
}

// ---------- exports ----------
const exportsInfo = { present: false, entries: [] };
{
  const d = dir(0);
  if (d.rva) {
    const p = R(d.rva); exportsInfo.present = true; exportsInfo.dllName = cstr(R(u32(p + 12))); exportsInfo.ordinalBase = u32(p + 16);
    const nf = u32(p + 20), nn = u32(p + 24), af = u32(p + 28), an = u32(p + 32), ao = u32(p + 36);
    const names = new Map();
    for (let i = 0; i < nn; i++) { const idx = u16(R(ao) + i * 2); (names.get(idx) || names.set(idx, []).get(idx)).push(cstr(R(u32(R(an) + i * 4)))); }
    for (let i = 0; i < nf; i++) { const rva = u32(R(af) + i * 4); if (rva) exportsInfo.entries.push({ ordinal: exportsInfo.ordinalBase + i, names: names.get(i) || [], rva: hex(rva) }); }
  }
}

// ---------- relocations ----------
const relocs = { directoryPresent: false, relocsStrippedFlag: !!(chars & 1), dynamicBase: !!(dllChars & 0x40) };
{
  const d = dir(5);
  if (d.rva && d.size) {
    relocs.directoryPresent = true; relocs.directoryRva = hex(d.rva); relocs.directorySize = d.size;
    const typeCount = {}; let blocks = 0, entries = 0, p = R(d.rva), end = p + d.size; const pages = [];
    while (p + 8 <= end) {
      const page = u32(p), sz = u32(p + 4); if (sz < 8) break; blocks++;
      for (let q = p + 8; q + 2 <= p + sz; q += 2) { const e = u16(q), t = e >> 12; typeCount[t] = (typeCount[t] || 0) + 1; if (t) entries++; }
      pages.push(page); p += sz;
    }
    relocs.blocks = blocks; relocs.fixups = entries; relocs.typeCounts = typeCount; relocs.typeNames = '3=HIGHLOW 0=ABSOLUTE(pad) 10=DIR64';
    relocs.firstPageRva = hex(Math.min(...pages)); relocs.lastPageRva = hex(Math.max(...pages));
    const rsec = secOfRva(d.rva); relocs.containedInSection = rsec ? rsec.name : null;
  }
}

// ---------- debug ----------
const debugTypes = { 0: 'UNKNOWN', 1: 'COFF', 2: 'CODEVIEW', 3: 'FPO', 4: 'MISC', 5: 'EXCEPTION', 6: 'FIXUP', 7: 'OMAP_TO_SRC', 8: 'OMAP_FROM_SRC', 9: 'BORLAND', 10: 'RESERVED10', 11: 'CLSID', 12: 'VC_FEATURE', 13: 'POGO', 14: 'ILTCG', 15: 'MPX', 16: 'REPRO', 20: 'EX_DLLCHARACTERISTICS' };
const debug = [];
{
  const d = dir(6);
  if (d.rva && d.size) for (let i = 0; i < d.size / 28; i++) {
    const p = R(d.rva) + i * 28; const type = u32(p + 12), size = u32(p + 16), fo = u32(p + 24), r = u32(p + 20);
    const e = { characteristics: u32(p), timeDateStamp: u32(p + 4), timeDateStampUtc: new Date(u32(p + 4) * 1000).toISOString(), majorVersion: u16(p + 8), minorVersion: u16(p + 10), type, typeName: debugTypes[type] || 'OTHER', sizeOfData: size, addressOfRawData: hex(r), pointerToRawData: hex(fo) };
    if (type === 2 && size >= 24 && b.toString('latin1', fo, fo + 4) === 'RSDS') {
      const g = b.subarray(fo + 4, fo + 20); const d1 = g.readUInt32LE(0).toString(16).padStart(8, '0'), d2 = g.readUInt16LE(4).toString(16).padStart(4, '0'), d3 = g.readUInt16LE(6).toString(16).padStart(4, '0');
      e.codeView = { signature: 'RSDS', guid: `${d1}-${d2}-${d3}-${g.subarray(8, 10).toString('hex')}-${g.subarray(10, 16).toString('hex')}`.toUpperCase(), age: u32(fo + 20), pdbPath: cstr(fo + 24, size - 24) };
    } else if (type === 2) e.codeView = { signature: b.toString('latin1', fo, fo + 4), note: 'non-RSDS (e.g. NB10); raw string: ' + cstr(fo + 16, size) };
    if (type === 13) e.pogoSignature = b.toString('latin1', fo, fo + 4);
    debug.push(e);
  }
}

// ---------- TLS ----------
const tls = { present: false };
{
  const d = dir(9);
  if (d.rva) {
    const p = R(d.rva); tls.present = true; tls.directoryRva = hex(d.rva); tls.directorySize = d.size;
    if (!is64) {
      tls.startRawDataVA = hex(u32(p)); tls.endRawDataVA = hex(u32(p + 4)); tls.addressOfIndexVA = hex(u32(p + 8)); tls.addressOfCallbacksVA = hex(u32(p + 12)); tls.sizeOfZeroFill = u32(p + 16); tls.characteristics = hex(u32(p + 20)); tls.templateBytes = u32(p + 4) - u32(p);
      tls.callbacks = []; const cb = u32(p + 12);
      if (cb) { for (let i = 0; i < 64; i++) { const v = u32(R(vaToRva(cb)) + i * 4); if (!v) break; const rva = vaToRva(v); tls.callbacks.push({ va: hex(v, 8), rva: hex(rva), section: secOfRva(rva)?.name || null }); } }
    }
  }
}

// ---------- load config ----------
const loadConfig = { present: false };
{
  const d = dir(10);
  if (d.rva) {
    const p = R(d.rva); loadConfig.present = true; loadConfig.declaredSize = u32(p); loadConfig.directorySize = d.size;
    if (!is64) {
      const sz = u32(p); const g = (off, n) => (sz >= off + 4 ? u32(p + off) : null);
      loadConfig.timeDateStamp = u32(p + 4); loadConfig.version = `${u16(p + 8)}.${u16(p + 10)}`;
      loadConfig.globalFlagsClear = hex(u32(p + 12)); loadConfig.globalFlagsSet = hex(u32(p + 16));
      if (sz >= 0x40) loadConfig.securityCookieVA = hex(u32(p + 0x3c));
      if (sz >= 0x48) { loadConfig.seHandlerTableVA = hex(u32(p + 0x40)); loadConfig.seHandlerCount = u32(p + 0x44); }
      if (sz >= 0x4c) loadConfig.guardCFCheckFunctionPointer = hex(u32(p + 0x48));
      if (sz >= 0x5c) { loadConfig.guardFlags = hex(u32(p + 0x58)); loadConfig.guardCFFunctionCount = u32(p + 0x54); }
    }
  }
}

// ---------- security (Authenticode) ----------
const security = (() => { const d = dir(4); return { present: !!d.rva, fileOffset: hex(d.rva), size: d.size }; })();

// ---------- resources ----------
const resourceTypeNames = { 1: 'CURSOR', 2: 'BITMAP', 3: 'ICON', 4: 'MENU', 5: 'DIALOG', 6: 'STRING', 9: 'ACCELERATOR', 10: 'RCDATA', 12: 'GROUP_CURSOR', 14: 'GROUP_ICON', 16: 'VERSION', 24: 'MANIFEST' };
const resources = { present: false, leaves: [] };
{
  const d = dir(2);
  if (d.rva) {
    resources.present = true; const base = R(d.rva);
    const name = (off) => { if (!(off & 0x80000000)) return off; const p = base + (off & 0x7fffffff); const n = u16(p); return b.toString('utf16le', p + 2, p + 2 + n * 2); };
    const walk = (off, level, path) => {
      const p = base + off; const nN = u16(p + 12), nI = u16(p + 14);
      for (let i = 0; i < nN + nI; i++) {
        const nm = u32(p + 16 + i * 8), of = u32(p + 20 + i * 8); const id = name(nm);
        if (of & 0x80000000) walk(of & 0x7fffffff, level + 1, [...path, id]);
        else { const q = base + of; resources.leaves.push({ type: path[0], typeName: resourceTypeNames[path[0]] || String(path[0]), id: path[1] ?? id, lang: level === 2 ? id : path[2], rva: hex(u32(q)), size: u32(q + 4) }); }
      }
    };
    walk(0, 0, []);
  }
}
function parseVersion() {
  const leaf = resources.leaves.find(l => l.type === 16); if (!leaf) return null;
  const off = R(parseInt(leaf.rva, 16)); const end = off + leaf.size;
  const node = (p) => {
    const len = u16(p), vlen = u16(p + 2), type = u16(p + 4); let q = p + 6; let key = ''; while (u16(q)) { key += String.fromCharCode(u16(q)); q += 2; } q += 2; q = (q + 3) & ~3;
    return { len, vlen, type, key, valOff: q };
  };
  const root = node(off); const out = { fixed: null, strings: {}, translations: [] };
  if (u32(root.valOff) === 0xfeef04bd) {
    const f = root.valOff; const v = (a, bb) => `${u16(f + bb)}.${u16(f + a)}`;
    out.fixed = { fileVersion: `${u16(f + 10)}.${u16(f + 8)}.${u16(f + 14)}.${u16(f + 12)}`, productVersion: `${u16(f + 18)}.${u16(f + 16)}.${u16(f + 22)}.${u16(f + 20)}`, fileFlagsMask: hex(u32(f + 24)), fileFlags: hex(u32(f + 28)), fileOS: hex(u32(f + 32)), fileType: hex(u32(f + 36)) };
  }
  let p = (root.valOff + root.vlen + 3) & ~3; const rootEnd = off + root.len;
  while (p < rootEnd && p < end) {
    const n = node(p); if (!n.len) break; const nEnd = p + n.len;
    if (n.key === 'StringFileInfo') {
      let t = n.valOff; while (t < nEnd) { const tb = node(t); if (!tb.len) break; const tEnd = t + tb.len; let s = tb.valOff; out.strings.langCodepage = tb.key;
        while (s < tEnd) { const sn = node(s); if (!sn.len) break; let val = ''; let vq = sn.valOff; while (u16(vq)) { val += String.fromCharCode(u16(vq)); vq += 2; } out.strings[sn.key] = val; s = (s + sn.len + 3) & ~3; }
        t = (t + tb.len + 3) & ~3; }
    } else if (n.key === 'VarFileInfo') { out.translations.push('present'); }
    p = (p + n.len + 3) & ~3;
  }
  return out;
}
let versionInfo = null; try { versionInfo = parseVersion(); } catch (e) { versionInfo = { error: String(e.message) }; }
let manifestXml = null;
{
  const m = resources.leaves.find(l => l.type === 24);
  if (m) { const f = R(parseInt(m.rva, 16)); manifestXml = b.toString('utf8', f, f + m.size).replace(/\r/g, ''); }
}

// ---------- import categorization ----------
const cats = [
  ['timing', /^(QueryPerformance(Counter|Frequency)|timeGetTime|timeBeginPeriod|timeEndPeriod|timeGetDevCaps|timeSetEvent|timeKillEvent|timeGetSystemTime|GetTickCount(64)?|GetSystemTime(AsFileTime|PreciseAsFileTime)?|GetLocalTime|GetSystemTimeAdjustment|Sleep(Ex)?|SwitchToThread|GetProcessTimes|GetThreadTimes|GetCurrentFiber|SetWaitableTimer|CreateWaitableTimer\w*|CreateTimerQueue\w*|SetTimer|KillTimer|FileTimeTo\w+|SystemTimeTo\w+|GetTimeZoneInformation|GetSystemTimes|QueryUnbiasedInterruptTime|GetTimeFormat\w|GetDateFormat\w|_time64?|_ftime\w*|clock|time|mciSendString\w*)$/],
  ['input', /^(XInput\w*|DirectInput\w*|DirectInput8Create|GetKeyboardState|GetKeyState|GetAsyncKeyState|GetKeyboardLayout\w*|MapVirtualKey\w*|VkKeyScan\w*|ToUnicode\w*|GetRawInput\w*|RegisterRawInputDevices|GetCursorPos|SetCursorPos|ClipCursor|ShowCursor|GetCapture|SetCapture|ReleaseCapture|joy\w+|GetKeyNameText\w*|SetKeyboardState|GetMouseMovePointsEx|TrackMouseEvent|RegisterDeviceNotification\w*|SetupDi\w+|HidD_\w+|HidP_\w+|GetFocus|SetFocus|GetForegroundWindow|GetActiveWindow|ImmGet\w+|ImmAssociateContext|ImmSetCompositionWindow|SendInput|keybd_event|mouse_event|BlockInput|GetLastInputInfo|LoadKeyboardLayout\w*|ActivateKeyboardLayout|GetUpdateRect)$/],
  ['graphics', /^(Direct3DCreate9\w*|D3DX\w+|D3D\w+|CreateDXGIFactory\w*|DXGI\w+|ChangeDisplaySettings\w*|EnumDisplay\w+|GetDeviceGammaRamp|SetDeviceGammaRamp|GetDC|ReleaseDC|CreateCompatible\w+|BitBlt|StretchBlt|StretchDIBits|SetDIBits\w*|GetDIBits|CreateDIBSection|SwapBuffers|wgl\w+|ChoosePixelFormat|SetPixelFormat|DescribePixelFormat|GetMonitorInfo\w*|MonitorFrom\w+|DwmEnableComposition|DwmIsCompositionEnabled|DwmFlush|CreateDC\w*|DeleteDC|GetDeviceCaps|UpdateLayeredWindow|SetLayeredWindowAttributes|CreateFontIndirect\w|CreateFont\w|TextOut\w|DrawText\w|GetTextExtent\w+|SelectObject|DeleteObject|SetWindowRgn|CreateRectRgn|InvalidateRect|BeginPaint|EndPaint|UpdateWindow|GetClientRect|GetWindowRect)$/],
  ['audio', /^(waveOut\w+|waveIn\w+|midiOut\w+|mixer\w+|DirectSound\w+|DirectSoundCreate\w*|DirectSoundEnumerate\w*|XAudio\w+|CreateAudioCaptureClient|mmioOpen\w*|mmio\w+|PlaySound\w*|sndPlaySound\w*|auxGetNumDevs|mciSendCommand\w*|CoCreateInstance\w*|X3DAudio\w+|XACT\w+|alc\w+|al[A-Z]\w+|OpenAL\w+)$/],
  ['threading_sync', /^(CreateThread|ExitThread|_beginthread\w*|_endthread\w*|SuspendThread|ResumeThread|TerminateThread|GetCurrentThread(Id)?|SetThreadPriority|GetThreadPriority|SetThreadAffinityMask|SetProcessAffinityMask|GetProcessAffinityMask|SetThreadIdealProcessor|SetThreadPriorityBoost|GetNumberOfProcessors|GetSystemInfo|GetNativeSystemInfo|Create(Event|Mutex|Semaphore)\w*|Open(Event|Mutex|Semaphore)\w*|SetEvent|ResetEvent|PulseEvent|ReleaseMutex|ReleaseSemaphore|WaitFor\w+|MsgWaitFor\w+|SignalObjectAndWait|(Initialize|Enter|Leave|Delete|TryEnter)CriticalSection\w*|InitializeSRWLock|Acquire\w*SRWLock\w*|Release\w*SRWLock\w*|SleepConditionVariable\w+|WakeConditionVariable|WakeAllConditionVariable|InitializeConditionVariable|Interlocked\w+|Tls(Alloc|Free|GetValue|SetValue)|Fls\w+|Get(Exit)?CodeThread\w*|QueueUserWorkItem|QueueUserAPC|CreateRemoteThread|DuplicateHandle|GetCurrentProcessId|GetCurrentProcess|SetThreadContext|GetThreadContext|CreateFiber|SwitchToFiber|ConvertThreadToFiber\w*|SetThreadExecutionState|Wow64\w+|OpenThread|SetThreadStackGuarantee|GetThreadTimes|CreateIoCompletionPort|GetQueuedCompletionStatus\w*|PostQueuedCompletionStatus|CreateThreadpool\w+|InitOnce\w+)$/],
  ['networking', /^(WSA\w+|socket|closesocket|connect|bind|listen|accept|send|sendto|recv|recvfrom|select|ioctlsocket|setsockopt|getsockopt|getsockname|getpeername|shutdown|gethostbyname|gethostbyaddr|gethostname|getaddrinfo|freeaddrinfo|getnameinfo|inet_\w+|htons|htonl|ntohs|ntohl|WSAGetLastError|__WSAFDIsSet|getservbyname|getprotobyname|GetAdaptersInfo|GetAdaptersAddresses|GetIpAddrTable|GetIfTable|IcmpSendEcho|IcmpCreateFile|InternetOpen\w*|InternetConnect\w*|HttpOpenRequest\w*|HttpSendRequest\w*|InternetReadFile|InternetCloseHandle|InternetCrack\w+|InternetSetOption\w*|InternetQueryOption\w*|InternetGetConnectedState\w*|URLDownloadToFile\w*|WinHttp\w+|Dns\w+|NetUser\w+|NetApi\w+|Net\w+|SendARP|GetBestInterface\w*|GetNumberOfInterfaces|GetTcpTable|GetUdpTable|WSAStartup|WSACleanup|XNet\w+|XOnline\w+|XLive\w+|XUser\w+|XNotify\w+|XLiveInitialize|XLiveInput|XLiveRender|XLiveUninitialize|XLivePreTranslateMessage|XLivePBufferAllocate|XLiveProtectedVerifyFile|XLiveSetSponsorToken|XLiveSetDebugTestMapping|XLivePBufferGetByteArray|XLivePBufferSetByteArray|XLiveProtectData|XLiveUnprotectData|XLiveCreateProtectedDataContext|XLiveCloseProtectedDataContext|XLiveQueryProtectedDataInformation)$/],
  ['steam', /^(Steam\w+|SteamAPI_\w+|SteamGameServer\w*|Steam_\w+|SteamClient\w*)$/],
  ['dynamic_loading', /^(LoadLibrary\w*|FreeLibrary|GetProcAddress|GetModuleHandle\w*|GetModuleFileName\w*|LdrLoadDll|LdrGetProcedureAddress|LoadLibraryEx\w*|SetDllDirectory\w*|AddDllDirectory|SetDefaultDllDirectories|DelayLoad\w+|__delayLoad\w+|__pfnDliNotifyHook2|__pfnDliFailureHook2|GetModuleHandleExW|GetModuleHandleExA|DisableThreadLibraryCalls)$/],
  ['anti_tamper_debug', /^(IsDebuggerPresent|CheckRemoteDebuggerPresent|OutputDebugString\w*|DebugBreak|NtQueryInformationProcess|NtSetInformationThread|NtQuerySystemInformation|ZwQuery\w+|NtQueryInformationThread|GetTickCount|CreateToolhelp32Snapshot|Process32\w+|Module32\w+|Thread32\w+|Heap32\w+|VirtualProtect\w*|VirtualQuery\w*|FlushInstructionCache|ReadProcessMemory|WriteProcessMemory|OpenProcess|SetUnhandledExceptionFilter|UnhandledExceptionFilter|AddVectoredExceptionHandler|RemoveVectoredExceptionHandler|RaiseException|CryptAcquireContext\w*|CryptCreateHash|CryptHashData|CryptGetHashParam|CryptDestroyHash|CryptReleaseContext|CryptVerifySignature\w*|CryptImportKey|CryptGenRandom|CryptDecrypt|CryptEncrypt|CryptHashSessionKey|CryptDeriveKey|CryptGetUserKey|WinVerifyTrust|CertGetCertificateChain\w*|CryptQueryObject|CryptMsg\w+|Cert\w+|GetVolumeInformation\w*|GetComputerName\w*|GetUserName\w*|RegQueryValueEx\w*|RegOpenKeyEx\w*|GetFileVersionInfo\w*|VerQueryValue\w*|GetCurrentHwProfile\w*|GetAdaptersInfo|GlobalMemoryStatus\w*|IsProcessorFeaturePresent|GetSystemFirmwareTable|EnumSystemFirmwareTables|EnumWindows|FindWindow\w*|GetWindowThreadProcessId)$/],
  ['crt_memory_runtime', /^(malloc|free|calloc|realloc|_aligned_\w+|_msize|_expand|memcpy|memmove|memset|memcmp|memchr|strlen|strcmp|strcpy|strncpy|strcat|strstr|strchr|strtol|strtod|atoi|atof|sprintf|_snprintf|vsprintf|_vsnprintf|printf|fprintf|fopen|fclose|fread|fwrite|fseek|ftell|fflush|rand|srand|qsort|bsearch|abort|exit|_exit|atexit|_onexit|__dllonexit|_amsg_exit|_cexit|_c_exit|_initterm\w*|_controlfp\w*|_clearfp|_statusfp\w*|_fpreset|_set_\w+|_invalid_parameter\w*|_CIpow|_CIsin|_CIcos|_CItan|_CIsqrt|_CIatan2|_CIfmod|_CIexp|_CIlog|_ftol\w*|_chkstk|_alloca_probe\w*|__CxxFrameHandler\w*|_except_handler\w+|__security_\w+|_purecall|\?\?[\w@?$]+|_beginthread\w*|HeapAlloc|HeapFree|HeapReAlloc|HeapCreate|HeapDestroy|HeapSize|HeapValidate|HeapCompact|HeapSetInformation|HeapLock|HeapUnlock|HeapWalk|VirtualAlloc\w*|VirtualFree\w*|GlobalAlloc|GlobalFree|GlobalLock|GlobalUnlock|GlobalReAlloc|LocalAlloc|LocalFree|GetProcessHeap|GetProcessHeaps|GetStartupInfo\w|GetCommandLine\w|GetEnvironment\w+|FreeEnvironmentStrings\w|SetEnvironmentVariable\w|GetEnvironmentVariable\w|SetLastError|GetLastError|InterlockedIncrement|EncodePointer|DecodePointer|_lock|_unlock|_errno|_getptd|__p__\w+|__set_app_type|__getmainargs|__wgetmainargs|_XcptFilter|__argc|__argv|_acmdln|_environ|_fmode|_commode|__initenv|_crt_debugger_hook|_encode_pointer|_decode_pointer|_set_SSE2_enable|__crtGetStringTypeW|_CrtDbgReport\w*|_ismbb\w+|__lc_codepage|__mb_cur_max|_tolower|_toupper|tolower|toupper|isalpha|isdigit|isspace|isalnum|setlocale|localeconv|__crtLCMapStringA|__crtLCMapStringW)$/],
];
const dllCats = [['graphics', /^(d3d\w*|d3dx\w*|dxgi|ddraw|opengl32|glu32|dwmapi|gdi32|d3dcompiler\w*|nvapi\w*|nvcuda\w*|amd\w*)\.dll$/i], ['input', /^(xinput\w*|dinput\w*|hid|setupapi|winmm_input)\.dll$/i], ['audio', /^(xaudio\w*|dsound|x3daudio\w*|xactengine\w*|openal\w*|winmm|msacm32|avrt|ksuser|mmdevapi|audioses|wmvcore|ole32_audio)\.dll$/i], ['networking', /^(ws2_32|wsock32|wininet|winhttp|iphlpapi|dnsapi|netapi32|mswsock|urlmon|wldap32|xlive|xnet|gamespy\w*|upnp\w*)\.dll$/i], ['steam', /^(steam_api\w*|steamclient\w*|steamworks\w*|gameoverlayrenderer\w*)\.dll$/i], ['physx', /^(physx\w*|nxcooking\w*|nxcharacter\w*|apex\w*|physxcore\w*|physxloader\w*|physxdevice\w*|cudart\w*|nvcuda\w*|nvcpl\w*)\.dll$/i], ['bink', /^(binkw32|bink\w*|smackw32|radvideo\w*|mss32|miles\w*|fmod\w*|wwise\w*)\.dll$/i], ['crt', /^(msvc[pr]\d+\w*|vcruntime\w*|ucrtbase|api-ms-win-crt-\w+|msvcrt)\.dll$/i]];
const physxBinkRe = [['physx', /^(Nx\w+|NxCreatePhysicsSDK|NxGetPhysicsSDKVersion|PhysX\w*|PxCreate\w+|nx\w+)$/], ['bink', /^(Bink\w+|_?Bink\w+@\d+|BinkOpen|BinkClose|BinkDoFrame|BinkNextFrame|BinkWait|BinkCopyToBuffer|BinkSetSoundSystem|BinkSetVolume|BinkPause|BinkGoto)\w*$/]];
function categorize(imp) {
  const out = new Set();
  for (const [c, re] of dllCats) if (re.test(imp.dll)) out.add(c);
  for (const s of imp.symbols) {
    const n = s.name; if (n === null) { out.add('ordinal_only'); continue; }
    for (const [c, re] of [...cats, ...physxBinkRe]) if (re.test(n)) { if (c === 'crt_memory_runtime' && /^(HeapAlloc|HeapFree|VirtualAlloc|VirtualFree|GetProcessHeap|GlobalAlloc|GlobalFree|LocalAlloc|LocalFree)/.test(n)) { /* memory API counted as crt/mem for browsing only */ } out.add(c); }
  }
  return [...out];
}
const catalog = {};
const addCat = (c, dll, sym) => { (catalog[c] ||= {}); (catalog[c][dll] ||= []).push(sym); };
for (const im of imports) {
  const dl = im.dll.toLowerCase();
  for (const s of im.symbols) {
    const label = s.name ?? `ordinal#${s.ordinal}`;
    if (s.name === null) { addCat('ordinal_only', im.dll, label); }
    for (const [c, re] of [...cats, ...physxBinkRe]) if (s.name && re.test(s.name)) addCat(c, im.dll, label);
    for (const [c, re] of dllCats) if (re.test(im.dll) && c !== 'crt' ) { /* whole-DLL membership */ catalog[`dll:${c}`] ||= {}; if (!catalog[`dll:${c}`][im.dll]) catalog[`dll:${c}`][im.dll] = []; catalog[`dll:${c}`][im.dll].push(label); }
  }
}
// dedupe per-category lists
for (const c of Object.keys(catalog)) for (const d of Object.keys(catalog[c])) catalog[c][d] = [...new Set(catalog[c][d])];

// ---------- strings ----------
function scanStrings() {
  const out = [];
  const s = b.toString('latin1');
  for (const m of s.matchAll(/[\x20-\x7e]{5,}/g)) out.push({ t: m[0], off: m.index, enc: 'a' });
  for (let par = 0; par < 2; par++) {
    let st = -1;
    for (let q = par; q + 1 < b.length; q += 2) {
      if (b[q] >= 32 && b[q] <= 126 && b[q + 1] === 0) { if (st < 0) st = q; }
      else { if (st >= 0 && q - st >= 10) out.push({ t: b.toString('utf16le', st, q), off: st, enc: 'w' }); st = -1; }
    }
  }
  return out;
}
const allStrings = scanStrings();
const strCats = {
  dllNames: /^[\w\-.]{1,40}\.(dll|exe|ocx|drv)$/i,
  xinput_dinput: /xinput|dinput|directinput|joystick|gamepad|hid\.dll/i,
  engine_unreal: /unreal|\bUE3\b|ue3|engine\.|\bEngine version|\bGEngine|netmode|\.upk\b|\.xxx\b|Binaries|UDK|GameSpy|\bUTGame|\bMKGame|MK_Game|Core\.dll|ued3/i,
  game_identity: /mortal|kombat|\bMK9\b|\bMKKE\b|netherrealm|midway|warner|WB Games|\bNRS\b|komplete/i,
  build_version: /\bbuild\b|changelist|\bCL[ -]?\d{4,}|version\s*[:=]?\s*\d|\b\d+\.\d+\.\d+\.\d+\b|\b(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9]\d \d{4}\b|compiled|\\Development\\|\\Src\\|\bP4\b|perforce/i,
  steam: /steam/i,
  network: /winsock|socket|upnp|netdriver|netconnection|ipdrv|onlinesubsystem|\bSTUN\b|nat type|matchmak|lobby|\bpacket\b|\bUDP\b|\bTCP\b|dedicated server/i,
  drm_tamper: /drm|securom|starforce|denuvo|arxan|steamstub|\.bind\b|solidshield|protect|anti.?cheat|tamper|punkbuster|\bGFWL\b|games for windows|xlive|licen[sc]e|activation|integrity/i,
  physx_bink_audio: /physx|\bnx[A-Z]\w+|bink|\bapex|fmod|wwise|xaudio|xact|dsound|openal/i,
  pdb_paths: /\.pdb\b|\.pch\b|[a-z]:\\[\w\\. -]*\.(cpp|h|c)\b/i,
  timing_hints: /fixed.?time|time ?step|\bdelta ?time\b|frame ?rate|\bfps\b|vsync|tick|timegettime|performance counter|max.?fps|smooth.?frame|fixed.?frame/i,
  input_config: /bindings|keybind|\bbutton\b|\bdpad\b|\bpad\b|controller|input/i,
};
const dynLoad = ['xinput1_1.dll', 'xinput1_2.dll', 'xinput1_3.dll', 'xinput1_4.dll', 'xinput9_1_0.dll', 'dinput8.dll', 'dinput.dll', 'steam_api.dll', 'xlive.dll', 'winmm.dll', 'ws2_32.dll', 'd3d9.dll', 'd3dx9_', 'physxloader', 'binkw32.dll', 'xaudio2_', 'xaudio', 'XInputGetState', 'XInputSetState', 'XInputGetCapabilities', 'XInputEnable', 'XInputGetKeystroke', 'XInputGetBatteryInformation', 'XInputGetStateEx', 'DirectInput8Create', 'SteamAPI_Init', 'timeBeginPeriod', 'QueryPerformanceCounter', 'GetProcAddress', 'LoadLibrary', 'xlive'];
const capped = (re, cap) => { const seen = new Set(), r = []; let n = 0; for (const s of allStrings) { if (!re.test(s.t)) continue; n++; const k = s.t.toLowerCase(); if (seen.has(k)) continue; seen.add(k); if (r.length < cap) { const rva = (() => { const sct = sections.find(x => s.off >= x.rawOffset && s.off < x.rawOffset + x.rawSize); return sct ? sct.rva + s.off - sct.rawOffset : null; })(); r.push({ text: s.t.length > 200 ? s.t.slice(0, 200) + '...' : s.t, fileOffset: s.off, rva: rva === null ? null : hex(rva, 8), section: rva === null ? null : (secOfRva(rva)?.name || null), enc: s.enc === 'a' ? 'ascii' : 'utf16le' }); } } return { totalMatches: n, uniqueMatches: seen.size, returned: r.length, records: r }; };
const stringsOut = { totalRuns: allStrings.length, scanNote: 'ASCII >=5 chars and printable-ASCII UTF-16LE >=5 chars; bounded per category', categories: {} };
for (const [k, re] of Object.entries(strCats)) stringsOut.categories[k] = capped(re, k === 'dllNames' ? 200 : 80);
// exact-ish lookups for dynamic-load strings with xrefs
const findAll = (needleBuf, from = 0) => { const r = []; let p = from; for (;;) { const i = b.indexOf(needleBuf, p); if (i < 0) break; r.push(i); p = i + 1; } return r; };
const xrefs = [];
const fileOffToRva = off => { const sct = sections.find(x => off >= x.rawOffset && off < x.rawOffset + x.rawSize); return sct ? sct.rva + off - sct.rawOffset : null; };
const relocSet = new Set();
if (relocs.directoryPresent) { const d = dir(5); let p = R(d.rva); const end = p + d.size; while (p + 8 <= end) { const page = u32(p), sz = u32(p + 4); if (sz < 8) break; for (let q = p + 8; q + 2 <= p + sz; q += 2) { const e = u16(q); if (e >> 12 === 3) relocSet.add(page + (e & 0xfff)); } p += sz; } }
for (const needle of dynLoad) {
  for (const enc of ['ascii', 'utf16le']) {
    const nb = Buffer.from(needle, enc === 'ascii' ? 'latin1' : 'utf16le');
    for (const off of findAll(nb)) {
      // require it to be a string start: previous byte(s) non-printable terminator
      const prev = enc === 'ascii' ? b[off - 1] : b[off - 1] === 0 && b[off - 2] === 0 ? 0 : (b[off - 1] === 0 ? 1 : 0);
      const startsClean = enc === 'ascii' ? !(prev >= 0x20 && prev <= 0x7e) : (off % 2 === 0 ? !(b[off - 2] >= 0x20 && b[off - 2] <= 0x7e && b[off - 1] === 0) : true);
      if (!startsClean && !/^(LoadLibrary|GetProcAddress|d3dx9_|xaudio2_|xaudio)/.test(needle)) continue;
      const rva = fileOffToRva(off); if (rva === null) continue;
      // full string
      let full; if (enc === 'ascii') full = cstr(off, 200); else { let e = off; while (e + 1 < b.length && (b[e] || b[e + 1]) && e - off < 400) e += 2; full = b.toString('utf16le', off, e); }
      // code xrefs: 4-byte absolute VA in other places; flag those listed in reloc table
      const va = imageBase + rva; const vb = Buffer.alloc(4); vb.writeUInt32LE(va >>> 0);
      const refs = findAll(vb).map(fo => { const r = fileOffToRva(fo); return { fileOffset: fo, rva: r === null ? null : hex(r, 8), section: r === null ? null : (secOfRva(r)?.name || null), inRelocTable: r !== null && relocSet.has(r) }; }).slice(0, 12);
      xrefs.push({ needle, encoding: enc, string: full, fileOffset: off, rva: hex(rva, 8), va: hex(va, 8), section: secOfRva(rva)?.name || null, absoluteVaRefsFound: refs.length, refs });
    }
  }
}
// dedupe identical records
{ const seen = new Set(); for (let i = xrefs.length - 1; i >= 0; i--) { const k = `${xrefs[i].fileOffset}|${xrefs[i].encoding}`; if (seen.has(k)) xrefs.splice(i, 1); else seen.add(k); } }

// ---------- section interpretation ----------
const epRva = u32(o + 16), epSec = secOfRva(epRva);
const secFlags = [];
for (const s of sections) {
  if (/^\.bind$/i.test(s.name)) secFlags.push(`${s.name}: Steam CEG/SteamStub DRM wrapper section name`);
  if (s.entropy > 7.2 && s.rawSize > 4096) secFlags.push(`${s.name}: entropy ${s.entropy} (>7.2: encrypted/compressed?)`);
  if (/^(upx\d|\.aspack|\.adata|\.nsp\d|\.petite|\.themida|\.vmp\d|\.enigma\d|\.packed|\.mpress\d|\.perplex|\.arma|\.securom|\.sdata|\.stub|\.text1)$/i.test(s.name)) secFlags.push(`${s.name}: name resembles packer/protector`);
  if (s.rawSize === 0 && s.virtualSize > 0 && !/bss|data/i.test(s.name)) secFlags.push(`${s.name}: virtual-only section`);
  if (s.characteristics & 0x20000000 && s.characteristics & 0x80000000) secFlags.push(`${s.name}: writable+executable`);
}
if (epSec && !(epSec.characteristics & 0x20000000)) secFlags.push('entry point section not executable');
const knownNames = new Set(['.text', '.rdata', '.data', '.idata', '.rsrc', '.reloc', '.tls', '.pdata', '.bss', '.edata', 'CODE', 'DATA', 'BSS']);
const nonStd = sections.filter(s => !knownNames.has(s.name)).map(s => s.name);

// ---------- steam acf ----------
let steam = null;
const acf = opt('--acf');
if (acf) { try { const txt = fs.readFileSync(acf, 'utf8'); steam = {}; for (const k of ['appid', 'name', 'installdir', 'buildid', 'TargetBuildID', 'LastUpdated', 'SizeOnDisk', 'StateFlags']) { const m = txt.match(new RegExp(`"${k}"\\s+"([^"]*)"`)); if (m) steam[k] = m[1]; } const dm = txt.match(/"InstalledDepots"\s*\{\s*"(\d+)"\s*\{\s*"manifest"\s+"(\d+)"/); if (dm) { steam.depot = dm[1]; steam.depotManifest = dm[2]; } if (steam.LastUpdated) steam.LastUpdatedUtc = new Date(+steam.LastUpdated * 1000).toISOString(); steam.source = acf; } catch (e) { steam = { error: e.message }; } }

// ---------- assemble ----------
const stat = fs.statSync(exePath);
const result = {
  _doc: 'All sections are directly measured from file bytes unless the key says heuristic.',
  file: { path: exePath, name: exePath.split(/[\\/]/).pop(), size: b.length, mtimeUtc: stat.mtime.toISOString(), hashes },
  steam,
  dos: { e_lfanew: hex(peOff), dosStubAndRichRegionBytes: peOff - 0x40 },
  coff, optional, checksum,
  dataDirectories: dirs,
  sections: sections.map(s => ({ name: s.name, rva: hex(s.rva, 8), virtualSize: s.virtualSize, rawOffset: hex(s.rawOffset, 8), rawSize: s.rawSize, entropy: s.entropy, characteristics: s.characteristicsHex, flags: s.characteristicsFlags, endRva: hex(s.endRva, 8) })),
  sectionAnalysis: { entryPointRva: hex(epRva, 8), entryPointSection: epSec?.name || null, overlayStartOffset: overlayStart, overlayBytes: Math.max(0, b.length - overlayStart), nonStandardSectionNames: nonStd, flags: secFlags, hasBindSection: sections.some(s => /^\.bind$/i.test(s.name)) },
  relocations: relocs,
  debug, tls, loadConfig, security, boundImports,
  richHeader: rich,
  exports: exportsInfo,
  resources: { present: resources.present, leaves: resources.leaves, versionInfo, manifestXml },
  imports: imports.map(im => ({ dll: im.dll, kind: im.kind, count: im.symbols.length, ordinalOnlyCount: im.symbols.filter(s => s.name === null).length, categories: categorize(im), timeDateStamp: im.timeDateStamp, descriptorRva: im.descriptorRva, firstThunk: im.firstThunk, symbols: im.symbols })),
  importCatalog: catalog,
  strings: stringsOut,
  dynamicLoadStringXrefs: xrefs,
};
const json = JSON.stringify(result, null, 2);
const out = opt('--out');
if (out) { fs.mkdirSync(out.replace(/[\\/][^\\/]*$/, ''), { recursive: true }); fs.writeFileSync(out, json + '\n'); console.error('wrote', out); } else console.log(json);
