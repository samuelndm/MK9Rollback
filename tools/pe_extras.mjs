// pe_extras.mjs - augments build_identity.json (produced by pe_info.mjs) with
//   * ordinalResolution: names for ordinal-only imports, resolved against the local C:\Windows\SysWOW64 DLL exports
//   * notableStrings: targeted engine/timing/DRM string lookups
//   * xinputLoadSite: byte-level decode of the code that references the "xinput1_3.dll" string
// Read-only on every input. Usage: node pe_extras.mjs <build_identity.json>
import fs from 'node:fs';
import crypto from 'node:crypto';

const jsonPath = process.argv[2];
const j = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
const exe = fs.readFileSync(j.file.path);
const hex = (n, w = 0) => '0x' + Number(n).toString(16).toUpperCase().padStart(w, '0');

// ---- minimal export parser for a 32-bit system DLL ----
function exportsOf(path) {
  const b = fs.readFileSync(path);
  const pe = b.readUInt32LE(0x3c), nsec = b.readUInt16LE(pe + 6), osz = b.readUInt16LE(pe + 20), o = pe + 24;
  const magic = b.readUInt16LE(o), dd = o + (magic === 0x20b ? 112 : 96);
  const secs = Array.from({ length: nsec }, (_, i) => { const s = o + osz + i * 40; return { va: b.readUInt32LE(s + 12), vs: b.readUInt32LE(s + 8), ro: b.readUInt32LE(s + 20), rs: b.readUInt32LE(s + 16) }; });
  const R = rva => { for (const s of secs) if (rva >= s.va && rva < s.va + Math.max(s.vs, s.rs)) return s.ro + rva - s.va; throw new Error('unmapped'); };
  const er = b.readUInt32LE(dd); if (!er) return { sha256: crypto.createHash('sha256').update(b).digest('hex'), map: new Map() };
  const e = R(er), base = b.readUInt32LE(e + 16), nf = b.readUInt32LE(e + 20), nn = b.readUInt32LE(e + 24), af = b.readUInt32LE(e + 28), an = b.readUInt32LE(e + 32), ao = b.readUInt32LE(e + 36);
  const cs = o2 => { let x = o2; while (b[x]) x++; return b.toString('latin1', o2, x); };
  const map = new Map();
  for (let i = 0; i < nn; i++) { const idx = b.readUInt16LE(R(ao) + i * 2); map.set(base + idx, cs(R(b.readUInt32LE(R(an) + i * 4)))); }
  for (let i = 0; i < nf; i++) { const ord = base + i; const rva = b.readUInt32LE(R(af) + i * 4); if (rva && !map.has(ord)) map.set(ord, '(ordinal-only)'); else if (rva) { /* named */ } }
  return { sha256: crypto.createHash('sha256').update(b).digest('hex').toUpperCase(), map };
}
const ordinalResolution = [];
for (const im of j.imports) {
  const ords = im.symbols.filter(s => s.name === null); if (!ords.length) continue;
  const path = `C:\\Windows\\SysWOW64\\${im.dll.toLowerCase()}`;
  const rec = { dll: im.dll, localReferenceDll: path, symbols: [] };
  try { const ex = exportsOf(path); rec.localReferenceSha256 = ex.sha256; for (const s of ords) rec.symbols.push({ ordinal: s.ordinal, iatRva: s.iatRva, resolvedName: ex.map.get(s.ordinal) || null }); }
  catch (e) { rec.error = String(e.message); for (const s of ords) rec.symbols.push({ ordinal: s.ordinal, iatRva: s.iatRva, resolvedName: null }); }
  ordinalResolution.push(rec);
}
j.ordinalResolution = { note: 'Names resolved by export ordinal against the locally installed 32-bit system DLL (Windows 11 SysWOW64). Ordinals of WS2_32/WSOCK32/OLEAUT32/DSOUND/XINPUT1_3 are ABI-stable, but the name mapping is externally derived, not stored in MKKE.exe.', entries: ordinalResolution };

// ---- targeted strings ----
const secs = j.sections.map(s => ({ ...s, va: parseInt(s.rva, 16), ro: parseInt(s.rawOffset, 16) }));
const off2rva = off => { const s = secs.find(x => off >= x.ro && off < x.ro + x.rawSize); return s ? s.va + off - s.ro : null; };
const needles = ['Build: ', 'GEngineVersion', 'GBuiltFromChangeList', 'UnrealEngine3', 'unreal-v', 'MaxSmoothedFrameRate', 'bSmoothFrameRate', 'MinSmoothedFrameRate', 'FixedFrameRate', 'bUseFixedFrameRate', 'UseVsync', 'VSync', 'GETMAXTICKRATE', 'MinDesiredFrameRate', 'Framerate: ', 'TickSimulation', 'DevTick', 'steam_appid', '-nosteam', 'NvOptimusEnablement', 'MKKE', 'DiscContent', 'PhysXLocal', 'PhysXLoader', 'MKLauncher', 'xlive', 'GFWL', 'Games for Windows', 'HVS_Agora', 'ReplayFrames', 'Rollback', 'rollback', 'DesyncHash', 'FrameNumber', 'GetSpectatorOutgoingDataSinceFrame', 'MKOnlineGamepadData', 'StopSendingGamepadData', 'mwNet', 'FMOD', 'SteamAPI_Init', 'Retail'];
const notable = [];
for (const n of needles) {
  const hits = []; let p = 0; const nb = Buffer.from(n, 'latin1');
  for (;;) { const i = exe.indexOf(nb, p); if (i < 0) break; p = i + 1;
    const rva = off2rva(i); if (rva === null) continue;
    const s = secs.find(x => rva >= x.va && rva < x.va + x.rawSize);
    if (s.name === '.text') continue; // skip incidental opcode matches
    let a = i; while (a > 0 && exe[a - 1] >= 0x20 && exe[a - 1] <= 0x7e) a--; let e = i; while (e < exe.length && exe[e] >= 0x20 && exe[e] <= 0x7e) e++;
    hits.push({ rva: hex(rva, 8), section: s.name, text: exe.toString('latin1', a, Math.min(e, a + 160)) });
    if (hits.length >= 6) break; }
  if (hits.length) notable.push({ needle: n, hits });
}
j.notableStrings = notable;

// ---- xinput load site decode ----
const xin = j.dynamicLoadStringXrefs.find(x => x.needle === 'xinput1_3.dll' && x.refs.length);
if (xin) {
  const ref = xin.refs[0]; const refRva = parseInt(ref.rva, 16);
  const text = secs.find(s => s.name === '.text'); const imageBase = parseInt(j.optional.imageBase, 16);
  const w0 = refRva - 0x90, w1 = refRva + 0x40;
  const iat = {}; for (const im of j.imports) for (const s of im.symbols) if (s.name) iat[parseInt(s.iatRva, 16)] = `${im.dll}!${s.name}`;
  const events = [];
  for (let r = w0; r < w1; r++) {
    const f = text.ro + r - text.va;
    const b0 = exe[f];
    if (b0 === 0x68) { const v = exe.readUInt32LE(f + 1); const rv = v - imageBase; const so = secs.find(s => rv >= s.va && rv < s.va + s.rawSize); if (so && so.name === '.rdata') { const fo = so.ro + rv - so.va; let e = fo; while (exe[e] && e - fo < 64) e++; const str = exe.toString('latin1', fo, e); if (str.length >= 4 && /^[\x20-\x7e]+$/.test(str)) events.push({ at: hex(r, 8), op: 'push imm32 (string)', operand: hex(v, 8), string: str }); } }
    else if (b0 === 0xFF && exe[f + 1] === 0x15) { const v = exe.readUInt32LE(f + 2) - imageBase; if (iat[v]) events.push({ at: hex(r, 8), op: 'call [IAT]', target: iat[v] }); }
    else if ((b0 === 0x8B && (exe[f + 1] & 0xC7) === 0x05) || (b0 === 0x8B && (exe[f + 1] & 0xC7) === 0x05)) { const v = exe.readUInt32LE(f + 2) - imageBase; if (iat[v]) events.push({ at: hex(r, 8), op: 'mov reg,[IAT]', target: iat[v] }); }
    else if (b0 === 0xFF && (exe[f + 1] === 0xD7 || exe[f + 1] === 0xD3)) events.push({ at: hex(r, 8), op: exe[f + 1] === 0xD7 ? 'call edi' : 'call ebx' });
  }
  const fo = text.ro + w0 - text.va;
  j.xinputLoadSite = { method: 'manual byte-pattern decode of x86 around the code reference to the "xinput1_3.dll" string (not a disassembler; operands verified against IAT and .rdata)', stringRva: xin.rva, codeReferenceRva: ref.rva, windowRva: [hex(w0, 8), hex(w1, 8)], windowHex: exe.subarray(fo, fo + (w1 - w0)).toString('hex'), events,
    interpretation: 'Sequence is mov edi,[LoadLibraryA]; push "d3d9.dll"; call edi; push <name>; push hModule; call [GetProcAddress]; ... push "d3dx9_43.dll"; call edi; ... push "xinput1_3.dll"; call edi(LoadLibraryA); test eax,eax; jz ...; push eax; call ebx(FreeLibrary). Reading: startup dependency probe (load then free); xinput1_3 is NOT resolved via GetProcAddress here. Interpretation of events list is inference from the decoded opcodes + register convention (edi/ebx) visible in the window.' };
}
fs.writeFileSync(jsonPath, JSON.stringify(j, null, 2) + '\n');
console.error('augmented', jsonPath, { ordinalDlls: ordinalResolution.length, notable: notable.length, xinputSite: !!j.xinputLoadSite });
