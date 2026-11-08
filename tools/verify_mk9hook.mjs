// verify_mk9hook.mjs
// Read-only verifier: checks every hard-coded address used by ermaccer/MK9Hook (v0.3.3 / 0.3.3-fix, "latest Steam exe")
// against OUR MKKE.exe. Never writes to the exe. Output: JSON next to docs/mk9hook_findings.md.
//
// usage: node verify_mk9hook.mjs [exePath] [outJson]
import fs from 'node:fs';
import crypto from 'node:crypto';
import path from 'node:path';

const exePath = process.argv[2] ?? 'D:\\Projects\\MK9\\analysis\\artifacts\\DiscContentPC\\MKKE.exe';
const outPath = process.argv[3] ?? 'D:\\Projects\\MK9\\MK9Rollback\\docs\\mk9hook_verification.json';
const EXPECT_SHA = 'D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6';

const buf = fs.readFileSync(exePath); // read-only
const sha = crypto.createHash('sha256').update(buf).digest('hex').toUpperCase();

// ---------- PE parsing ----------
const peOff = buf.readUInt32LE(0x3c);
if (buf.toString('latin1', peOff, peOff + 4) !== 'PE\0\0') throw new Error('bad PE');
const nSec = buf.readUInt16LE(peOff + 6);
const optSize = buf.readUInt16LE(peOff + 20);
const opt = peOff + 24;
const magic = buf.readUInt16LE(opt);
const imageBase = buf.readUInt32LE(opt + 28);
const entryRva = buf.readUInt32LE(opt + 16);
const sizeOfImage = buf.readUInt32LE(opt + 56);
const dllChars = buf.readUInt16LE(opt + 70);
const relocDir = { rva: buf.readUInt32LE(opt + 96 + 5 * 8), size: buf.readUInt32LE(opt + 96 + 5 * 8 + 4) };
const sections = [];
for (let i = 0; i < nSec; i++) {
  const o = opt + optSize + i * 40;
  const name = buf.toString('latin1', o, o + 8).replace(/\0.*$/, '');
  const vsize = buf.readUInt32LE(o + 8), rva = buf.readUInt32LE(o + 12);
  const rsize = buf.readUInt32LE(o + 16), rptr = buf.readUInt32LE(o + 20);
  const ch = buf.readUInt32LE(o + 36);
  sections.push({ name, vsize, rva, rsize, rptr, ch,
    exec: !!(ch & 0x20000000), write: !!(ch & 0x80000000), read: !!(ch & 0x40000000),
    initData: !!(ch & 0x40), uninit: !!(ch & 0x80),
    va: imageBase + rva });
}
const pe = { magic: '0x' + magic.toString(16), isPE32: magic === 0x10b, imageBase: '0x' + imageBase.toString(16),
  entryVA: '0x' + (imageBase + entryRva).toString(16), sizeOfImage, aslrDynamicBase: !!(dllChars & 0x40),
  hasRelocDir: relocDir.size > 0, sections: sections.map(s => ({ name: s.name, va: '0x' + s.va.toString(16),
    vsize: s.vsize, rawPtr: s.rptr, rawSize: s.rsize, exec: s.exec, write: s.write })) };

const hex = (n, w = 8) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(w, '0');
function secOf(va) {
  const rva = va - imageBase;
  return sections.find(s => rva >= s.rva && rva < s.rva + Math.max(s.vsize, s.rsize)) ?? null;
}
function v2o(va) { // VA -> file offset, or null if not file-backed
  const s = secOf(va); if (!s) return null;
  const d = va - imageBase - s.rva;
  if (d >= s.rsize) return null; // BSS tail
  return s.rptr + d;
}
function bytesAt(va, n) {
  const out = [];
  for (let i = 0; i < n; i++) { const o = v2o(va + i); out.push(o == null ? null : buf[o]); }
  return out;
}
const hx = a => a.map(b => b == null ? '??' : b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
const text = sections.find(s => s.name === '.text') ?? sections.find(s => s.exec);
const execSecs = sections.filter(s => s.exec);

// ---------- scan helpers ----------
function scanExecForValue32(val) { // absolute imm32 / disp32 occurrences inside executable sections
  const hits = [];
  const b0 = val & 0xff, b1 = (val >>> 8) & 0xff, b2 = (val >>> 16) & 0xff, b3 = (val >>> 24) & 0xff;
  for (const s of execSecs) {
    const end = s.rptr + Math.min(s.rsize, s.vsize) - 3;
    for (let o = s.rptr; o < end; o++) {
      if (buf[o] === b0 && buf[o + 1] === b1 && buf[o + 2] === b2 && buf[o + 3] === b3)
        hits.push({ va: imageBase + s.rva + (o - s.rptr), prev: buf[o - 1], prev2: buf[o - 2] });
    }
  }
  return hits;
}
// relative call/jmp index: target -> [site VAs]
const callIdx = new Map(), jmpIdx = new Map();
for (const s of execSecs) {
  const end = s.rptr + Math.min(s.rsize, s.vsize) - 4;
  for (let o = s.rptr; o < end; o++) {
    const op = buf[o];
    if (op === 0xE8 || op === 0xE9) {
      const site = imageBase + s.rva + (o - s.rptr);
      const tgt = (site + 5 + buf.readInt32LE(o + 1)) >>> 0;
      const t = secOf(tgt);
      if (t && t.exec) (op === 0xE8 ? callIdx : jmpIdx).set(tgt, [...((op === 0xE8 ? callIdx : jmpIdx).get(tgt) ?? []), site]);
    }
  }
}
// NB: byte-wise E8 scan includes false positives (E8 inside other instructions). Counts are an upper bound
// but for real function entries they are dominated by true calls.

function classifyPrologue(b) {
  const h = hx(b.slice(0, 8));
  if (b[0] === 0x55 && b[1] === 0x8B && b[2] === 0xEC) return ['STRONG', 'push ebp; mov ebp,esp'];
  if (b[0] === 0x6A && b[1] === 0xFF && b[2] === 0x68) return ['STRONG', 'SEH frame (push -1; push handler)'];
  if (b[0] === 0x83 && b[1] === 0xEC) return ['STRONG', 'sub esp,imm8'];
  if (b[0] === 0x81 && b[1] === 0xEC) return ['STRONG', 'sub esp,imm32'];
  if (b[0] === 0x64 && b[1] === 0xA1) return ['STRONG', 'mov eax,fs:[..]'];
  if ([0x51, 0x52, 0x53, 0x56, 0x57].includes(b[0])) return ['PLAUSIBLE', 'push reg (callee-save/thiscall spill)'];
  if (b[0] === 0x8B && (b[1] === 0x44 || b[1] === 0x4C || b[1] === 0x54) && b[2] === 0x24) return ['PLAUSIBLE', 'mov reg,[esp+x] (arg load, leaf)'];
  if (b[0] === 0x8B && b[1] >= 0xC0) return ['PLAUSIBLE', 'mov reg,reg'];
  if (b[0] === 0xF3 && b[1] === 0x0F) return ['PLAUSIBLE', 'SSE scalar op (float leaf)'];
  if (b[0] === 0xD9 || b[0] === 0xD8) return ['PLAUSIBLE', 'x87 op (float leaf)'];
  if (b[0] === 0x33 || b[0] === 0x31) return ['PLAUSIBLE', 'xor reg,reg'];
  if (b[0] === 0xB8 || b[0] === 0xB9) return ['PLAUSIBLE', 'mov reg,imm32'];
  if (b[0] === 0xA1 || (b[0] === 0x8B && (b[1] & 0xC7) === 0x05)) return ['PLAUSIBLE', 'load global'];
  if (b[0] === 0x8B && (b[1] & 0xC0) === 0x40) return ['PLAUSIBLE', 'mov reg,[reg+disp8] (thiscall leaf)'];
  if (b[0] === 0x8B && (b[1] & 0xC0) === 0x80) return ['PLAUSIBLE', 'mov reg,[reg+disp32] (thiscall leaf)'];
  if (b[0] === 0xE9) return ['PLAUSIBLE', 'jmp thunk'];
  if (b[0] === 0xC7 || b[0] === 0x89 || b[0] === 0x8D) return ['PLAUSIBLE', 'mov/lea'];
  return ['UNKNOWN', h];
}
function paddingBefore(va) {
  const p = bytesAt(va - 4, 4);
  const last = p[3];
  if (last === 0xCC) return 'CC-pad';
  if (last === 0xC3) return 'after-RET';
  if (p[1] === 0xC2 && last === 0x00) return 'after-RET imm16';
  if (last === 0x90) return 'NOP-pad';
  if (p[2] === 0xC2) return 'after-RET imm16?';
  if (last === 0x00 && p[2] === 0x00) return 'zero-pad';
  return 'no-pad (' + hx(p) + ')';
}
function firstRet(va, max = 0x120) {
  // heuristic: first C3 / C2 xx 00 after va (may be a false hit inside an operand)
  const b = bytesAt(va, max);
  for (let i = 0; i < max; i++) {
    if (b[i] === 0xC3) return { off: i, kind: 'ret (cdecl/caller-clean or thiscall no-arg)', pops: 0 };
    if (b[i] === 0xC2 && b[i + 2] === 0x00 && b[i + 1] !== null && b[i + 1] % 4 === 0 && b[i + 1] <= 0x40)
      return { off: i, kind: 'ret ' + b[i + 1], pops: b[i + 1] };
  }
  return null;
}

// ---------- the catalogue (everything hard-coded in MK9Hook) ----------
const items = [];
const add = (o) => items.push(o);
const F = (name, va, conv, sig, purpose, src, extra = {}) => add({ name, va, kind: 'func', conv, sig, purpose, src, ...extra });
const D = (name, va, dtype, purpose, src, extra = {}) => add({ name, va, kind: 'data', dtype, purpose, src, ...extra });

// -- globals
D('GFG_GAME_INFO', 0xE89B70, 'ptr', 'MK9Hook treats it as a pointer to the game-info object (*(int*)0xE89B70 passed as ECX to GetInfo); in this exe it is the address of the static FG_GAME_INFO singleton itself', 'code/mk9.h:7, mk9.cpp:12');
D('ms_fGameSpeed (SetGameSpeed)', 0xE91E24, 'float', 'global time-dilation scalar; slow-mo writes *(float*)0xE91E24 = speed (default 1.0, UI clamps 0..2)', 'code/mk9.cpp:129');
D('Game name string ("Unreal")', 0xD15624, 'string:Unreal', 'CheckGame(): version gate, string compared to "Unreal"', 'dllmain.cpp:132');
D('colorFilter (FVector RGB)', 0xDEBF8C, 'vec3', 'screen color-filter/tint vector (R,G,B floats)', 'code/mk9menu.cpp:183');
D('ms_fHudScaleX', 0xDE9078, 'float', 'HUD scale X; HUD hide = *= -1', 'code/mk9menu.cpp:184');
D('ms_fHudScaleY', 0xDE907C, 'float', 'HUD scale Y; HUD hide = *= -1', 'code/mk9menu.cpp:185');
D('ms_bPauseGame', 0xE43054, 'bool', 'bool& to pause flag (declared, never read/written elsewhere in MK9Hook)', 'code/mk9menu.cpp:186');

// -- plain game functions
F('GetInfo (player-info accessor)', 0x80E540, '__thiscall', 'int(*)(int gameinfo, PLAYER_NUM plr) ; ECX=*(GFG_GAME_INFO), stack arg=plr', 'returns PlayerInfo* for PLAYER1..4 (enum is 0-based: PLAYER1=0); everything else derives from it', 'code/mk9.cpp:13');
F('GetObj (info->pawn/obj, handle resolve)', 0x895B70, '__thiscall', 'int(*)(int info, int zero)', 'resolves live character object from a PlayerInfo (also used on backup/tag info = info+0xEC)', 'code/mk9.cpp:23,28');
F('SetCharacter', 0x8141A0, '__cdecl', 'void(*)(PLAYER_NUM, int isBackup, char* name)', 'overrides char selection for P1/P2 (unk=1 for tag partner)', 'code/mk9.cpp:33');
F('SetStage', 0x811DD0, '__cdecl', 'void(*)(const char* stage)', 'overrides stage name', 'code/mk9.cpp:38');
F('GetAIDrone (info->drone)', 0x44D480, '__thiscall', 'int(*)(int info)', 'returns AI drone object for a PlayerInfo', 'code/mk9.cpp:82');
F('SetAIScript (drone,script)', 0x891A80, '__cdecl', 'void(*)(int drone, const char* script)', 'assigns an AI script to a drone', 'code/mk9.cpp:84');
F('RunScript (MKScript fn on char script ctx)', 0x45C610, '__thiscall', 'void(*)(int ctx /*obj+0x2FA4*/, MKScript*, int fnId)', 'executes MK script function on a character (script VM!)', 'code/mk9.cpp:104');
F('AddKoins', 0x972A70, '__cdecl', 'void(*)(int info, int amount)', 'adds Koins to profile', 'code/mk9.cpp:194');
F('VersusScreen sprintf-like', 0x616500, '__cdecl', 'void(*)(int len, char* buf, char* fmt, char* charName, char* tex)', 'original callee of hooked call at 0x8146A3', 'code/mk9.cpp:322');
F('MKCamera::SetPosition', 0x45E1C0, '__thiscall', 'void(*)(MKCamera*, FVector*)', 'original camera position setter; 29 call sites hooked', 'code/mkcamera.cpp:11');
F('MKCamera::SetRotation', 0x45DB30, '__thiscall', 'void(*)(MKCamera*, FRotator*)', 'original camera rotation setter; 31 call sites hooked', 'code/mkcamera.cpp:17');
F('MKCamera::SetFOV', 0x45DAE0, '__thiscall', 'void(*)(MKCamera*, float FOV, int)', 'original FOV setter; 17 call sites hooked', 'code/mkcamera.cpp:23');
F('MKScript::GetFunctionID', 0x458680, '__thiscall', 'int(*)(MKScript*, char* name, int)', 'script function lookup by name/hash', 'code/GameInfo.cpp:5');
F('LoadScriptPackage', 0x611E80, '__cdecl', 'int(*)(int, const char* pkg)', 'load/get script package', 'code/GameInfo.cpp:10');
F('GetScript(package,name)', 0x48F850, '__cdecl', 'MKScript*(*)(int pkg, const char* name, int)', 'get script from package', 'code/GameInfo.cpp:14');
F('FName::FName ctor', 0x600980, '__thiscall', 'void(*)(FName*, const char*, EFindName, int)', 'UE3 FName constructor (name->index)', 'code/unreal/FName.cpp:5');
F('USkeletalMeshComponent::GetBoneLocation', 0x77C3B0, '__thiscall', 'void(*)(USkelMesh*, FVector*, int boneIdx, int, int)', 'bone world location', 'code/unreal/SkeletalMeshComponent.cpp:11');
F('USkeletalMeshComponent::GetBoneQuaternion', 0x77C2B0, '__thiscall', 'void(*)(USkelMesh*, FQuat*, int boneIdx, int, int)', 'bone quaternion', 'SkeletalMeshComponent.cpp:17');
F('USkeletalMeshComponent::GetBoneMatrix', 0x76E250, '__thiscall', 'void(*)(USkelMesh*, FMatrix*, int id, int space)', 'bone matrix', 'SkeletalMeshComponent.cpp:22');
F('SkeletalMesh::MatchRefBone (FindBoneIndex)', 0x779270, '__thiscall', 'int(*)(int skeleton /*this+640*/, int* ids)', 'bone-name->id lookup', 'SkeletalMeshComponent.cpp:38');
F('Dismember step 1', 0x77BD70, '__thiscall', 'void(*)(USkelMesh*)', 'dismemberment prep', 'SkeletalMeshComponent.cpp:45');
F('Dismember step 2', 0x766920, '__thiscall', 'void(*)(USkelMesh*, int partID)', 'dismember part', 'SkeletalMeshComponent.cpp:46');
F('FMatrix from quat/origin (commented out)', 0x5ED0E0, '__thiscall', 'FMatrix*(*)(FMatrix*, FQuat*, FVector*)', 'only in a comment', 'SkeletalMeshComponent.cpp:49', { commentedOut: true });

// -- patched call sites (InjectHook PATCH_CALL)
add({ name: 'Versus screen text call site', va: 0x8146A3, kind: 'callsite', expectTarget: 0x616500, conv: '__cdecl', purpose: 'redirect to HookLoadPlayerVersusScreen (then calls 0x616500)', src: 'dllmain.cpp:41' });
add({ name: 'SetupFight call site', va: 0x80DF82, kind: 'callsite', expectTarget: null, conv: '__cdecl (void,void)', purpose: 'redirect to SetupFight() each time a fight starts; MK9Hook hooks the FIGHT-START event here', src: 'dllmain.cpp:42' });
const camPos = [0x82A24D,0x86AAB7,0x86CC89,0x86D13C,0x86D1E7,0x86D2B6,0x86D6E8,0x86D8E0,0x86DDB4,0x873462,0x8743C3,0x875F2E,0x878A46,0x878FB4,0x879229,0x87B3F6,0x87B73D,0x87B872,0x87B8D7,0x87C756,0x87CA63,0x87D382,0x87D4E2,0x87D5C7,0x87D8C1,0x87DC44,0x87E42C,0x8DB93A,0x9782E2];
const camRot = [0x82A25B,0x86AAC7,0x86CC94,0x86D144,0x86D2C1,0x86D6F3,0x86D8EB,0x86DDBF,0x86EF00,0x86EF15,0x86EF2A,0x873470,0x876044,0x8760A1,0x8760FE,0x877B70,0x878A88,0x878FC2,0x87921E,0x87B401,0x87B732,0x87B87D,0x87B8E2,0x87CA6E,0x87D4ED,0x87D5D2,0x87D8C9,0x87DC4C,0x87E437,0x8DB96A,0x9782F0];
const camFov = [0x82A26E,0x82A2E9,0x86B5A3,0x86B5C6,0x86D34A,0x86DE3F,0x8761F9,0x876214,0x876264,0x87629C,0x8795D9,0x87B88D,0x87B8F2,0x87C6BF,0x87E44C,0x8DB8D1,0x978303];
camPos.forEach((va, i) => add({ name: `Camera SetPosition callsite #${i + 1}`, va, kind: 'callsite', expectTarget: 0x45E1C0, conv: '__thiscall', purpose: 'camera position update', src: `dllmain.cpp:${44 + i}`, group: 'camPos' }));
camRot.forEach((va, i) => add({ name: `Camera SetRotation callsite #${i + 1}`, va, kind: 'callsite', expectTarget: 0x45DB30, conv: '__thiscall', purpose: 'camera rotation update', src: `dllmain.cpp:${74 + i}`, group: 'camRot' }));
camFov.forEach((va, i) => add({ name: `Camera SetFOV callsite #${i + 1}`, va, kind: 'callsite', expectTarget: 0x45DAE0, conv: '__thiscall', purpose: 'camera FOV update', src: `dllmain.cpp:${107 + i}`, group: 'camFov' }));
add({ name: 'XInputGetState wrapper (PATCH_JUMP)', va: 0x99CA36, kind: 'jmpsite', conv: '__stdcall (DWORD, XINPUT_STATE*)', purpose: 'overwrites first 5 bytes with E9 -> XInputGetState_Hook; zeroes Gamepad while menu open', src: 'dllmain.cpp:127' });

// -- struct offsets (no pattern; assessed via disp32 operand census + hand-checks)
const OFFS = [
  ['PlayerInfo+0xC14', 0xC14, 'float meter (super meter 0..1)', 'mk9.cpp:44'],
  ['PlayerInfo+0xEC (236)', 0xEC, 'backup/tag partner PlayerInfo = info+236 (embedded)', 'mk9.cpp:18'],
  ['PlayerInfo+0x964 (2404)', 0x964, 'character name char[]', 'mk9.cpp:110'],
  ['PlayerInfo+0x17EC', 0x17EC, 'float damage (combo) scale', 'mk9.cpp:99'],
  ['Obj+0x78C0', 0x78C0, 'float life 0..1', 'mk9.cpp:50'],
  ['Obj+0xF4', 0xF4, 'float per-char speed multiplier', 'mk9.cpp:76'],
  ['Obj+0x2FA4', 0x2FA4, 'script context ptr (RunCharacterScript ECX)', 'mk9.cpp:104'],
  ['Obj+0x10C (268)', 0x10C, 'USkeletalMeshComponent*', 'mk9.cpp:124'],
  ['Actor+0x11C (0x118+4)', 0x11C, 'FVector scale', 'mk9.cpp:70'],
  ['Actor+0xDC', 0xDC, 'FVector location', 'mk9.cpp:117'],
  ['SkelMesh+0x280 (640)', 0x280, 'skeleton/ref-skel pointer', 'SkeletalMeshComponent.cpp:31'],
  ['Camera+0x118 (280)', 0x118, 'FRotator (matrix)', 'mkcamera.cpp:192'],
];
const smallOffs = [['Obj+0x8', 8, 'Actor* (pawn)', 'mk9.cpp:69'], ['PlayerInfo/Obj handle idx 0x1C/0x20', 0x1c, 'n/a', 'n/a']];

// Expected callee-cleanup (ret imm16) implied by MK9Hook's declared signature+convention.
// cdecl => 0 (caller cleans); thiscall/stdcall => 4 * (#stack args). Checked heuristically against RET opcodes in the body.
const EXPECT_POPS = new Map([
  [0x80E540, 4], [0x895B70, 4], [0x8141A0, 0], [0x811DD0, 0], [0x44D480, 0], [0x891A80, 0], [0x45C610, 8], [0x972A70, 0],
  [0x616500, 0], [0x45E1C0, 4], [0x45DB30, 4], [0x45DAE0, 8], [0x458680, 8], [0x611E80, 0], [0x48F850, 0], [0x600980, 12],
  [0x77C3B0, 16], [0x77C2B0, 16], [0x76E250, 12], [0x779270, 4], [0x77BD70, 0], [0x766920, 4], [0x5ED0E0, 8],
]);
// extent of a function = up to the first run of >=2 CC bytes after the entry (compiler padding). Collect RET forms inside.
function retForms(va, cap = 0x4000) {
  const b = bytesAt(va, cap); let end = cap;
  for (let i = 4; i < cap - 1; i++) if (b[i] === 0xCC && b[i + 1] === 0xCC) { end = i; break; }
  const pops = new Set(); let c3 = 0;
  for (let i = 0; i < end; i++) {
    if (b[i] === 0xC3) c3++;
    if (b[i] === 0xC2 && b[i + 2] === 0x00 && b[i + 1] != null && b[i + 1] <= 0x40 && b[i + 1] % 4 === 0) pops.add(b[i + 1]);
  }
  return { extentBytes: end, c3Count: c3, c2Imm: [...pops].sort((x, y) => x - y) };
}

// ---------- run checks ----------
const results = [];
for (const it of items) {
  const r = { name: it.name, kind: it.kind, va: hex(it.va), src: it.src, purpose: it.purpose, conv: it.conv ?? null, sig: it.sig ?? null, group: it.group ?? null };
  const s = secOf(it.va); r.section = s ? s.name : null;
  r.fileOffset = v2o(it.va);
  const bt = bytesAt(it.va, 32);
  r.bytes32 = hx(bt);
  const notes = [];
  let status = 'MISMATCH';

  if (it.kind === 'func') {
    const [grade, why] = classifyPrologue(bt);
    r.prologue = grade + ': ' + why;
    r.padBefore = paddingBefore(it.va);
    r.callSites = (callIdx.get(it.va) ?? []).length;
    r.jmpSites = (jmpIdx.get(it.va) ?? []).length;
    r.firstSites = (callIdx.get(it.va) ?? []).slice(0, 6).map(v => hex(v));
    const fr = firstRet(it.va); r.firstRet = fr;
    if (EXPECT_POPS.has(it.va)) {
      const rf = retForms(it.va); const want = EXPECT_POPS.get(it.va);
      r.retForms = rf; r.expectedCalleePops = want;
      const ok = want === 0 ? (rf.c3Count > 0 && rf.c2Imm.length === 0) : rf.c2Imm.includes(want);
      r.convCheck = ok ? (want === 0 ? 'CONSISTENT (only plain RET seen => caller-clean, cdecl-compatible)' : `CONSISTENT (RET ${want} seen => callee pops ${want}, thiscall/stdcall-compatible)`) : 'INCONSISTENT/UNCLEAR (heuristic: c2=' + JSON.stringify(rf.c2Imm) + ', c3=' + rf.c3Count + ')';
    }
    const inText = s && s.exec;
    const padOk = /pad|RET/.test(r.padBefore);
    if (!inText) status = 'MISMATCH', notes.push('not in executable section');
    else if (grade === 'UNKNOWN') status = 'AMBIGUOUS', notes.push('unrecognised prologue');
    else if (padOk && r.callSites > 0) status = 'MATCH', notes.push('function-start boundary + ' + r.callSites + ' call sites');
    else if (r.callSites > 0) status = 'MATCH', notes.push('plausible start, called ' + r.callSites + 'x, but no padding before');
    else if (padOk) status = 'AMBIGUOUS', notes.push('plausible start, no direct callers found (maybe vtable/indirect)');
    else status = 'AMBIGUOUS', notes.push('prologue ok but no padding and no callers');
    if (it.commentedOut) notes.push('only referenced in a comment in MK9Hook');
  } else if (it.kind === 'callsite') {
    const o = it.va; const b = bytesAt(o, 5);
    r.siteBytes = hx(b);
    const inText = s && s.exec;
    if (!inText) { status = 'MISMATCH'; notes.push('not in .text'); }
    else if (b[0] !== 0xE8) { status = 'MISMATCH'; notes.push('byte at VA is ' + hex(b[0], 2) + ', not E8'); }
    else {
      const rel = (b[1] | (b[2] << 8) | (b[3] << 16) | (b[4] << 24)) | 0;
      const tgt = (o + 5 + rel) >>> 0; r.target = hex(tgt);
      const ts = secOf(tgt); r.targetSection = ts?.name ?? null;
      if (it.expectTarget != null) {
        r.expectedTarget = hex(it.expectTarget);
        if (tgt === it.expectTarget) status = 'MATCH', notes.push('E8 -> expected ' + hex(tgt));
        else status = 'MISMATCH', notes.push('E8 but target ' + hex(tgt) + ' != expected ' + hex(it.expectTarget));
      } else {
        if (ts && ts.exec) status = 'MATCH', notes.push('E8 call to ' + hex(tgt) + ' (target not stated by MK9Hook; plausible)');
        else status = 'AMBIGUOUS', notes.push('E8 target not in exec section');
        const tb = bytesAt(tgt, 16); r.targetBytes = hx(tb); r.targetPrologue = classifyPrologue(tb).join(': ');
        r.targetPad = paddingBefore(tgt);
        r.targetCallers = (callIdx.get(tgt) ?? []).length;
      }
    }
  } else if (it.kind === 'jmpsite') {
    const b = bytesAt(it.va, 5); r.siteBytes = hx(b);
    const [grade, why] = classifyPrologue(bt);
    r.prologue = grade + ': ' + why; r.padBefore = paddingBefore(it.va);
    r.callSites = (callIdx.get(it.va) ?? []).length;
    if (!(s && s.exec)) { status = 'MISMATCH'; notes.push('not in exec section'); }
    else if (r.callSites > 0 && /pad|RET/.test(r.padBefore)) status = 'MATCH', notes.push('function entry w/ ' + r.callSites + ' callers (5-byte E9 overwrite spans ' + hx(bt.slice(0, 5)) + ')');
    else if (r.callSites > 0) status = 'AMBIGUOUS', notes.push('called ' + r.callSites + 'x but no clean boundary before');
    else status = 'AMBIGUOUS', notes.push('no direct callers');
    // does the code near it reference XInput import? count imports later
  } else if (it.kind === 'data') {
    const expectSecs = (s && !s.exec);
    r.sectionWritable = s?.write ?? null;
    const refs = scanExecForValue32(it.va);
    r.codeRefCount = refs.length;
    r.codeRefsFirst = refs.slice(0, 10).map(h => hex(h.va));
    const raw = (v2o(it.va) == null) ? null : bytesAt(it.va, 16);
    r.diskInitBytes16 = raw ? hx(raw) : '(not file-backed: .bss tail / zero-init)';
    if (it.dtype === 'float') {
      const o = v2o(it.va);
      if (o != null) { r.diskFloat = buf.readFloatLE(o); }
    }
    if (it.dtype.startsWith('string:')) {
      const want = it.dtype.slice(7);
      const o = v2o(it.va);
      const got = o != null ? buf.toString('latin1', o, o + 32).split('\0')[0] : null;
      r.stringAtVA = got;
      if (got === want) status = 'MATCH', notes.push('string == "' + want + '"');
      else status = 'MISMATCH', notes.push('string is ' + JSON.stringify(got));
    } else {
      if (!s) { status = 'MISMATCH'; notes.push('VA outside image'); }
      else if (s.exec) { status = 'MISMATCH'; notes.push('lies in exec section ' + s.name); }
      else if (refs.length >= 1) status = 'MATCH', notes.push(`in ${s.name}; referenced by ${refs.length} imm32 in code`);
      else status = 'AMBIGUOUS', notes.push(`in ${s.name}; no direct imm32 refs from code`);
      if (it.dtype === 'float' && r.diskFloat !== undefined) notes.push('disk init float=' + r.diskFloat);
      if (it.dtype === 'ptr') {
        const o = v2o(it.va);
        if (o != null) { const p = buf.readUInt32LE(o); notes.push('disk init ptr=' + hex(p)); }
      }
    }
  }
  r.status = status; r.notes = notes;
  results.push(r);
}

// ---------- deep hand-checks on key accessors ----------
const deep = {};
function disasmHint(va, n = 64) { return hx(bytesAt(va, n)); }
function findPattern(va, len, pat) { // pat: array of ints / null for wildcard; search in [va, va+len)
  const b = bytesAt(va, len); const hits = [];
  for (let i = 0; i + pat.length <= len; i++) { let ok = true; for (let j = 0; j < pat.length; j++) if (pat[j] != null && b[i + j] !== pat[j]) { ok = false; break; } if (ok) hits.push(i); }
  return hits;
}
const le32 = n => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
// GetInfo 0x80E540: expect base 0xE89D7C, stride 0xC8C (from prior independent analysis)
deep.GetInfo_0x80E540 = {
  bytes64: disasmHint(0x80E540, 64),
  contains_0xE89D7C_at: findPattern(0x80E540, 0x60, le32(0xE89D7C)),
  contains_0xC8C_at: findPattern(0x80E540, 0x60, le32(0xC8C)),
  firstRet: firstRet(0x80E540),
  note: 'matches feasibility.md: returns 0xE89D7C + idx*0xC8C; idx is the 0-based stack arg (MK9Hook PLAYER1=0); idx>=4 clamps to base; ECX unused; ret 4'
};
deep.GetObj_0x895B70 = { bytes96: disasmHint(0x895B70, 96), firstRet: firstRet(0x895B70),
  contains_1C: findPattern(0x895B70, 0x60, [0x1C]).length, note: 'combat-state-investigation.md: validates pointer at obj+0x1C and id at +0x20 against pointee +4 (handle resolver)' };
// GFG_GAME_INFO relationship
deep.GFG_vs_playerinfo_records = { GFG_GAME_INFO: hex(0xE89B70), records_base: hex(0xE89D7C), delta: hex(0xE89D7C - 0xE89B70), stride: hex(0xC8C),
  note: 'record base lies 0x20C bytes into the object that GFG_GAME_INFO points at / is located at; MK9Hook calls GetInfo with *(int*)GFG_GAME_INFO as this' };
// refs to the global game-info pointer, game speed, pause flag, with enclosing 64k area breakdown
function refsDetail(va) { const h = scanExecForValue32(va); return { va: hex(va), count: h.length, sites: h.slice(0, 24).map(x => hex(x.va) + ' (prev bytes ' + hex(x.prev2, 2) + ' ' + hex(x.prev, 2) + ')') }; }
deep.refs_GFG_GAME_INFO = refsDetail(0xE89B70);
deep.refs_GameSpeed = refsDetail(0xE91E24);
deep.refs_PauseFlag = refsDetail(0xE43054);
deep.refs_PlayerInfoBase = refsDetail(0xE89D7C);
deep.refs_RNGstate = refsDetail(0xE7C054);
deep.refs_TimePeriod = refsDetail(0xE7C010);
deep.refs_gate_E53BFC = refsDetail(0xE53BFC);
deep.refs_gate_E3B85C = refsDetail(0xE3B85C);
deep.refs_engine_E5873C = refsDetail(0xE5873C);

// Struct-offset census
deep.structOffsets = OFFS.map(([n, off, p, src]) => {
  const hits = scanExecForValue32(off).filter(h => (h.prev & 0xC0) === 0x80 || h.prev === 0x68 || h.prev === 0xB8); // [reg+disp32] modrm (mod=10) / push / mov
  const disp = hits.filter(h => (h.prev & 0xC0) === 0x80);
  return { symbol: n, offset: hex(off, 4), purpose: p, src, dispAccessCount: disp.length, firstSites: disp.slice(0, 8).map(h => hex(h.va)),
    caveat: 'census of [reg+disp32] operands equal to the offset anywhere in code; supportive only, not proof of ownership' };
});
// Does the offset occur inside the immediate neighbourhood (±0x200) of related functions?
function nearFn(va, off, win = 0x400) { return scanExecForValue32(off).filter(h => (h.prev & 0xC0) === 0x80 && Math.abs(h.va - va) < win).map(h => hex(h.va)); }

// Prior-finding cross check table
const prior = [
  ['0x0080E540', 0x80E540, 'player-info accessor (feasibility.md)', 'MK9Hook GetInfo'],
  ['0x00895B70', 0x895B70, 'handle/pointer validator at obj+0x1C/+0x20 (combat-state-investigation.md)', 'MK9Hook GetObj'],
  ['0x00424A50', 0x424A50, 'hardware input sampler', null],
  ['0x00404D30', 0x404D30, 'main application loop', null],
  ['0x004025A0', 0x4025A0, 'timed engine phases', null],
  ['0x0080F670', 0x80F670, 'engine virtual +0xC0 (C0)', null],
  ['0x0080DD40', 0x80DD40, 'engine virtual +0xBC (BC)', null],
  ['0x005F5080', 0x5F5080, 'RNG int generator', null],
  ['0x005F50A0', 0x5F50A0, 'RNG float generator', null],
  ['0x005ED570', 0x5ED570, 'seed setter', null],
  ['0x00E7C054', 0xE7C054, 'RNG state', null],
  ['0x00E43054', 0xE43054, 'pause-like flag (prior doc: getter source)', 'MK9Hook ms_bPauseGame'],
  ['0x00406790', 0x406790, 'getter of pause flag per prior doc', null],
  ['0x007F7F50', 0x7F7F50, 'C0 base routine', null],
];
deep.priorCrossCheck = prior.map(([lab, va, role, mk9]) => {
  const s = secOf(va), bt = bytesAt(va, 16);
  return { label: lab, role, mk9hookSymbol: mk9, section: s?.name, bytes16: hx(bt), callSites: (callIdx.get(va) ?? []).length, prologue: s?.exec ? classifyPrologue(bt).join(': ') : 'data' };
});
// who touches the pause flag: show nearest function entries
deep.pauseFlagNeighbourhood = {
  note: 'getter at 0x406790 per prior doc; dump its body to see whether it reads 0xE43054',
  bytes_0x406790: hx(bytesAt(0x406790, 24)),
};
// 0x4025A0 / 0x404D30 / 0x80F670: do any of them reference MK9Hook's gamespeed global (0xE91E24)?
function refsInRange(va, lo, hi) { return scanExecForValue32(va).filter(h => h.va >= lo && h.va < hi).map(h => hex(h.va)); }
deep.gameSpeedRefsInEngineLoop_0x4025A0_0x4026FF = refsInRange(0xE91E24, 0x4025A0, 0x402800);
deep.gameSpeedRefsInMainLoop_0x404D30 = refsInRange(0xE91E24, 0x404D30, 0x404F00);

// import check: XInput and d3d9
function imports() {
  const impDirRva = buf.readUInt32LE(opt + 96 + 1 * 8);
  const out = [];
  function rva2o(rva) { const s = sections.find(s => rva >= s.rva && rva < s.rva + Math.max(s.vsize, s.rsize)); return s ? s.rptr + (rva - s.rva) : null; }
  let p = rva2o(impDirRva);
  while (p != null) {
    const nameRva = buf.readUInt32LE(p + 12); if (!nameRva) break;
    const dll = buf.toString('latin1', rva2o(nameRva), rva2o(nameRva) + 64).split('\0')[0];
    const ft = buf.readUInt32LE(p + 16); const oft = buf.readUInt32LE(p) || ft;
    const fns = []; let q = rva2o(oft), idx = 0;
    while (q != null) {
      const v = buf.readUInt32LE(q); if (!v) break;
      if (!(v & 0x80000000)) fns.push({ name: buf.toString('latin1', rva2o(v) + 2, rva2o(v) + 66).split('\0')[0], iatVA: hex(imageBase + ft + idx * 4) });
      else fns.push({ name: '#' + (v & 0xffff), iatVA: hex(imageBase + ft + idx * 4) });
      q += 4; idx++;
    }
    out.push({ dll, count: fns.length, fns });
    p += 20;
  }
  return out;
}
const imps = imports();
deep.importsInteresting = imps.filter(i => /xinput|d3d9|dinput|winmm|kernel32|user32|ws2_32|dsound|xaudio|d3dx/i.test(i.dll)).map(i => ({
  dll: i.dll, count: i.count,
  wanted: i.fns.filter(f => /xinput/i.test(i.dll) || /XInputGetState|Direct3DCreate9|DirectInput8Create|QueryPerformanceCounter|GetTickCount|timeGetTime|CreateThread|GetAsyncKeyState|SetWindowsHook|ClipCursor/i.test(f.name))
}));
deep.allImportDlls = imps.map(i => i.dll + ' (' + i.count + ')');
const xi = imps.flatMap(i => i.fns.map(f => ({ dll: i.dll, ...f }))).find(f => f.name === 'XInputGetState');
if (xi) {
  const iat = parseInt(xi.iatVA, 16);
  const refs = scanExecForValue32(iat).map(h => hex(h.va));
  deep.xinputGetState = { iatVA: xi.iatVA, dll: xi.dll, codeRefs: refs, mk9hookPatchSite: '0x99CA36', note: 'MK9Hook patches an in-image wrapper/stub at 0x99CA36 with JMP -> hook, not the IAT; compare refs to IAT slot' };
} else deep.xinputGetState = { note: 'XInputGetState not found in import table (dynamic or different DLL)' };
// bytes around the XInput site
deep.xinputSiteContext = { bytes_0x99CA20: hx(bytesAt(0x99CA20, 64)) };

// XInput thunk decode: FF 25 <iat> -> which import?
{
  const b = bytesAt(0x99CA36, 6);
  const allFns = imps.flatMap(i => i.fns.map(f => ({ dll: i.dll, ...f })));
  if (b[0] === 0xFF && b[1] === 0x25) {
    const iat = b[2] | (b[3] << 8) | (b[4] << 16) | (b[5] << 24);
    const f = allFns.find(f => parseInt(f.iatVA, 16) === (iat >>> 0));
    deep.xinputThunk = { thunkVA: '0x0099CA36', bytes: hx(b), jmpTarget: 'dword ptr [' + hex(iat) + ']', importEntry: f ?? null,
      thunkCallers: (callIdx.get(0x99CA36) ?? []).map(hex),
      note: 'E9 patch writes 5 bytes over FF 25 xx xx xx; 6th byte (the high byte of the IAT address = 00) is left alone and is never executed',
      xinput1_3_ordinals: '2 = XInputGetState, 3 = XInputSetState (stock xinput1_3.dll export ordinals)' };
    const f2 = allFns.find(f => parseInt(f.iatVA, 16) === ((iat >>> 0) - 4));
    deep.xinputThunk.neighbourThunk = { va: '0x0099CA3C', importEntry: f2 ?? null };
  }
}
// samplers reaching it: sampler 0x424A50 contains call site 0x424B01 (-> 0x99CA36)
deep.xinputCallerContainment = (callIdx.get(0x99CA36) ?? []).map(site => ({ site: hex(site), insideSampler_0x424A50: site >= 0x424A50 && site < 0x424A50 + 0x400 }));

// Derived structure from hand-decoded key bodies (bytes preserved so the claim can be re-checked)
const win = (va, n, label) => ({ label, va: hex(va), bytes: hx(bytesAt(va, n)) });
deep.derivedWindows = [
  win(0x80E540, 36, 'GetInfo: if(idx>=4) return 0xE89D7C; else return idx*0xC8C+0xE89D7C; ret 4 (ECX unused)'),
  win(0x895B70, 32, 'GetObj: p=[ecx+0x1C]; null/0xDEADBEEF->0; id=[ecx+0x20]; id==-1 -> 0; [p+4]==id ? p : 0; ret 4'),
  win(0x44D480, 40, 'GetAIDrone: p=[ecx+0xBAC] / id=[ecx+0xBB0] handle pair, same sentinel scheme'),
  win(0x8949E0, 16, 'virtual float GetTimeScale(): fld [ecx+0xF4]; fmul [0xE91E24]; ret  (only reader of MK9Hook "game speed")'),
  win(0x406790, 12, 'getter: mov eax,[0xE43054]; ret   (pause-like flag; also 0x4067A0 -> [0xE43058])'),
  win(0x407AB0, 120, 'setter: SetPause(int) writes [0xE43054] (and [0xE43058]); 11 call sites'),
  win(0x80DF50, 64, 'FG_GAME_INFO::GameInit(): esi=this(0xE89B70); logs "FG_GAME_INFO::GameInit()" via 0x8FE4D0(0x10,str)'),
  win(0x8FE4D0, 32, '0x8FE4D0: conditional log helper: if([0xED66EC] & mask) vprintf-ish(fmt,args); cdecl; 1703 callers -- MK9Hook SetupFight REPLACES this log call (original not called)'),
  win(0x810100, 16, 'writes Obj+0x78C0 (life) then mirrors into +0x78C4 (round/health reset)'),
  win(0x424AF0, 24, 'inside sampler 0x424A50: push &state(esi+0xE0E34C); push idx; call 0x99CA36 (XInputGetState thunk)'),
  win(0x5F5080, 32, 'RNG int: eax=[0xE7C054]*0x0BB38435+0x3619636B; store back  (matches prior LCG)'),
];
deep.vtableSlots = {
  engine_vtable_0xD60D30: { plus_BC_slot: win(0xD60DEC, 8, 'slot +0xBC -> 0x80DD40'), plus_C0_slot: win(0xD60DF0, 8, 'slot +0xC0 -> 0x80F670') },
  timescale_getter_slot: win(0xD6B3E8, 8, 'vtable slot containing 0x8949E0 (GetTimeScale)'),
};
deep.derivedCallers = Object.fromEntries([0x407AB0, 0x8949E0, 0x80DF50, 0x424A50, 0x404D30, 0x4025A0, 0x5F5080, 0x5F50A0, 0x5ED570, 0x80E540, 0x895B70, 0x44D480, 0x45C610, 0x8141A0, 0x811DD0, 0x972A70].map(a => [hex(a), (callIdx.get(a) ?? []).length + ' direct E8 sites' + ((callIdx.get(a) ?? []).length && (callIdx.get(a) ?? []).length <= 12 ? ': ' + (callIdx.get(a)).map(v => hex(v)).join(' ') : '')]));
// immediate (any 4-byte) census for fields that scan as zero in the [reg+disp32] filter
deep.rawImm32 = Object.fromEntries([0x17EC, 0xC14, 0x78C0, 0x78C4, 0x2FA4, 0xBAC, 0xBB0, 0x964, 0xEC].map(v => [hex(v, 4), scanExecForValue32(v).length]));
deep.playerInfoStrideNote = 'PlayerInfo stride 0xC8C (3212 bytes). MK9Hook uses PlayerInfo+0x17EC for damage scale: 0x17EC > 0xC8C, so for P1 that address is inside PlayerInfo[1] at +0xB60 (and PlayerInfo[3] for P2). Zero 4-byte occurrences of 0x17EC in code => UNVERIFIED / suspicious.';

// ---------- summary ----------
const byStatus = {}; const byKind = {};
for (const r of results) {
  byStatus[r.status] = (byStatus[r.status] ?? 0) + 1;
  const k = r.kind + (r.group ? ':' + r.group : '');
  byKind[k] = byKind[k] ?? {}; byKind[k][r.status] = (byKind[k][r.status] ?? 0) + 1;
}
const out = {
  tool: 'verify_mk9hook.mjs', generatedAt: new Date().toISOString(),
  exe: { path: exePath, size: buf.length, sha256: sha, expectedSha256: EXPECT_SHA, shaMatches: sha === EXPECT_SHA },
  pe, summary: { total: results.length, byStatus, byKind },
  results, deep,
  mk9hook_patterns_note: 'MK9Hook (ermaccer) contains NO byte-pattern/signature scans; every address is a hard-coded absolute VA. "Pattern" verification was therefore done by prologue/padding/caller-census plus first-16-byte uniqueness (see uniqueness field in deep.uniqueness).'
};
// first-12-byte uniqueness of function entries (image-wide)
out.deep.uniqueness = results.filter(r => r.kind === 'func').map(r => {
  const va = parseInt(r.va, 16); const pat = bytesAt(va, 12);
  // skip bytes that are rel32/imm that vary? we keep exact; report match count
  let n = 0; const hits = [];
  for (const s of execSecs) {
    const end = s.rptr + Math.min(s.rsize, s.vsize) - 12;
    outer: for (let o = s.rptr; o < end; o++) {
      if (buf[o] !== pat[0]) continue;
      for (let j = 1; j < 12; j++) if (buf[o + j] !== pat[j]) continue outer;
      n++; if (hits.length < 4) hits.push(hex(imageBase + s.rva + (o - s.rptr)));
    }
  }
  return { name: r.name, va: r.va, first12: hx(pat), imageWideMatches: n, vas: hits };
});

fs.mkdirSync(path.dirname(outPath), { recursive: true });
fs.writeFileSync(outPath, JSON.stringify(out, null, 2));
console.log('exe sha256', sha, sha === EXPECT_SHA ? 'OK' : 'MISMATCH!');
console.log('imageBase', hex(imageBase), 'sections', sections.map(s => `${s.name}@${hex(s.va)}+${hex(s.vsize)}${s.exec ? 'X' : ''}${s.write ? 'W' : ''}`).join(' '));
console.log('summary', JSON.stringify(out.summary));
console.log('wrote', outPath);
