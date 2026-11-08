# MK9Hook reference findings (verified against our MKKE.exe)

Generated for MK9Rollback. Companion files: `tools/verify_mk9hook.mjs` (verifier), `docs/mk9hook_verification.json` (raw verifier output).
No MK9Hook code was copied into this project; this is a catalogue of facts (addresses, signatures, offsets) plus architecture notes.

## 1. Provenance and target build

| Item | Value |
|---|---|
| Repo | https://github.com/ermaccer/MK9Hook (default branch `main`) |
| Latest source state analysed | HEAD = commit `9d8d517` "typo" (2022-10-12); release tag **0.3.3-fix** (2022-10-12); `MK9HOOK_VERSION "0.3.3"` in `code/mk9.h` |
| Releases | 0.1 (2021-05-21), 0.2 (2021-12-19), 0.2.1 (2022-02-20), 0.3 (2022-03-26), 0.3.2 (2022-07-01), 0.3.3 (2022-09-27), 0.3.3-fix (2022-10-12). Each ships one `MK9Hook.zip` (~1.2-1.3 MB, contains Ultimate ASI Loader `dinput8.dll` + `MK9Hook.asi` + ini). Binaries were NOT downloaded. |
| Target exe | "latest Steam executable" (MessageBox text in `CheckGame()`); README says to drop files in `DiscContentPC`. No exe hash or build number is published. |
| Version gate | `CheckGame()` compares the string at VA `0xD15624` to `"Unreal"` (dllmain.cpp:132). On our exe that VA holds exactly `55 6E 72 65 61 6C 00` ("Unreal") **MATCH**. |
| Our exe | `MKKE.exe`, SHA-256 `D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6` (matches expected: true), PE32, image base `0x400000`, entry `0x98cb8f`, **no ASLR** (DYNAMIC_BASE off), has .reloc. Sections: .text@0x401000(+0x853fd4) X, .rdata@0xc55000(+0x18b64b), .data@0xde1000(+0x201ebc) W, .rsrc@0xfe3000(+0x168fc), .reloc@0xffa000(+0xb3b82). |
| Licensing | No license header seen in dllmain.cpp; check repo LICENSE before reusing any code. We only take facts. |
| Credits in README | Ultimate ASI Loader, ModUtils (MemoryMgr.h), Dear ImGui, kiero, ImGui-DX9-Kiero-Hook sample. |

**Fetch caveat.** Source was read with WebFetch (summarising proxy). `mk9.cpp`, `mkcamera.cpp`, `mk9menu.h` member list and `UpdateControls` came back verbatim; other files came back as summaries plus exact lines for the symbols grepped. Line numbers in the "Source" column are as reported by the proxy and may be off by a few lines. The catalogue of hex literals is believed complete: hex literals only occur in dllmain.cpp, mk9.h/.cpp, mk9menu.cpp (lines 183-186), mkcamera.cpp, GameInfo.cpp, SkeletalMeshComponent.cpp, FName.cpp (eGamepadManager/eDirectX9Hook/eSettingsManager contain none).

**MK9Hook uses NO byte patterns or signature scans.** Every address is a hard-coded absolute VA at image base 0x400000 (it cannot survive a different exe). "Pattern verification" was therefore done structurally: section, function-start boundary (CC/RET padding before), prologue class, direct-caller census, call-target decoding, ret-imm calling-convention consistency, plus hand-decoded bodies for the rollback-relevant ones.

## 2. Verification summary

- Total hard-coded VAs verified: **110** (7 data/globals, 23 functions incl. 1 only referenced from a comment, 2 named single call-site patches, 77 camera call sites, 1 jmp patch).
- Result: **MATCH 110 / MISMATCH 0 / AMBIGUOUS 0.** Every address is in the expected section; every function is a clean function start; all 77 camera call sites are `E8` calls hitting exactly the expected original (0x45E1C0 / 0x45DB30 / 0x45DAE0); all 23 calling conventions are consistent with ret/ret-imm opcodes (after correcting one of my own expectations, RunScript pops 8).
- Conclusion: **our MKKE.exe is the exact executable family MK9Hook 0.3.3 targets** (essentially certain; 110/110 including 77 independent call-site matches).
- Items MK9Hook uses that are NOT confirmed by this exe (struct-offset section 4): `PlayerInfo+0x17EC` (damage scale) is suspicious (see below). The `0xE91E24` game-speed default is not statically explained.

Key to "Verification": prologue grade (STRONG = push ebp/mov ebp,esp / sub esp / SEH frame; PLAUSIBLE = leaf code), "CC-pad"/"after-RET" = a clean function boundary before the VA, "direct E8 callers" = byte-scan of E8 rel32 into the VA (upper bound, includes rare false positives), conv = RET imm16 heuristic.

## 3. Catalogue

### 3.1 Globals / data

| Symbol | MK9Hook addr | Purpose | CallConv | Verification result | bytes | Source |
|---|---|---|---|---|---|---|
| GFG_GAME_INFO | `0x00E89B70` (.data) | MK9Hook treats it as a pointer to the game-info object (*(int*)0xE89B70 passed as ECX to GetInfo); in this exe it is the address of the static FG_GAME_INFO singleton itself | n/a (data: `FG_GAME_INFO` singleton (C++ object, NOT a pointer)) | **MATCH** - in .data; referenced by 482 imm32 in code. MK9Hook reads `*(int*)0xE89B70` as "gameinfo" and passes it as ECX to GetInfo; in the exe 0xE89B70 is `mov ecx, 0xE89B70` (this) at 482 sites, so MK9Hook actually passes the first dword (vtable ptr); harmless because GetInfo ignores ECX. PlayerInfo[0] = 0xE89B70 + 0x20C. | `?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??` | code/mk9.h:7, mk9.cpp:12 |
| ms_fGameSpeed (SetGameSpeed) | `0x00E91E24` (.data) | global time-dilation scalar; slow-mo writes *(float*)0xE91E24 = speed (default 1.0, UI clamps 0..2) | n/a (data: float time-scale multiplier) | **MATCH** - in .data; referenced by 1 imm32 in code. Single reader: `fmul [0xE91E24]` at 0x8949E8 inside virtual `GetTimeScale()` (0x8949E0). BSS (not file-backed); no imm32 store to it anywhere in code, so its default (1.0) is established at runtime by something not found statically -> confirm dynamically. | `?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??` | code/mk9.cpp:129 |
| Game name string ("Unreal") | `0x00D15624` (.rdata) | CheckGame(): version gate, string compared to "Unreal" | n/a (data: version gate) | **MATCH** - string == "Unreal". On-disk bytes `55 6E 72 65 61 6C 00` = "Unreal" exactly. | `55 6E 72 65 61 6C 00 00 4F 70 74 69 6F 6E 73 00` | dllmain.cpp:132 |
| colorFilter (FVector RGB) | `0x00DEBF8C` (.data) | screen color-filter/tint vector (R,G,B floats) | n/a (data: 3 floats (R,G,B)) | **MATCH** - in .data; referenced by 124 imm32 in code. Initial `1.0 1.0 1.0 1.0 ...`; 124 code refs. Plausible global colour-grade vector. | `00 00 80 3F 00 00 80 3F 00 00 80 3F 00 00 80 3F` | code/mk9menu.cpp:183 |
| ms_fHudScaleX | `0x00DE9078` (.data) | HUD scale X; HUD hide = *= -1 | n/a (data: float) | **MATCH** - in .data; referenced by 1 imm32 in code; disk init float=1. Initial 1.0; 1 code ref. HUD hidden by negating (draw scale < 0). | `00 00 80 3F 00 00 80 3F 18 A4 C5 00 18 A4 C5 00` | code/mk9menu.cpp:184 |
| ms_fHudScaleY | `0x00DE907C` (.data) | HUD scale Y; HUD hide = *= -1 | n/a (data: float) | **MATCH** - in .data; referenced by 1 imm32 in code; disk init float=1. Initial 1.0; 1 code ref. | `00 00 80 3F 18 A4 C5 00 18 A4 C5 00 18 A4 C5 00` | code/mk9menu.cpp:185 |
| ms_bPauseGame | `0x00E43054` (.data) | bool& to pause flag (declared, never read/written elsewhere in MK9Hook) | n/a (data: int32 pause-like flag (MK9Hook declares `bool&`)) | **MATCH** - in .data; referenced by 9 imm32 in code. Code uses `cmp dword [..]`, `mov [..],esi`: it is a 32-bit int. Getter 0x406790 = `mov eax,[0xE43054]; ret`; setter 0x407AB0; sibling flag 0xE43058 (getter 0x4067A0). Coincides with prior combat-state-investigation.md. | `?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??` | code/mk9menu.cpp:186 |

### 3.2 Game functions called by MK9Hook

All are raw casts like `((int(__thiscall*)(int,int))0x895B70)(...)`; no hooks are installed on them (they are only *called*), except those listed in 3.3.

| Symbol | MK9Hook addr | Purpose | CallConv / signature | Verification result | bytes (16) | Source |
|---|---|---|---|---|---|---|
| GetInfo (player-info accessor) | `0x0080E540` | returns PlayerInfo* for PLAYER1..4 (enum is 0-based: PLAYER1=0); everything else derives from it | `__thiscall` int(*)(int gameinfo, PLAYER_NUM plr) ; ECX=*(GFG_GAME_INFO), stack arg=plr | **MATCH** - STRONG prologue, CC-pad, 219 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 45 08 83 F8 04 7C 09 B8 7C 9D E8 00` | code/mk9.cpp:13 |
| GetObj (info->pawn/obj, handle resolve) | `0x00895B70` | resolves live character object from a PlayerInfo (also used on backup/tag info = info+0xEC) | `__thiscall` int(*)(int info, int zero) | **MATCH** - PLAUSIBLE prologue, CC-pad, 278 direct E8 callers; conv: CONSISTENT | `8B 41 1C 85 C0 74 14 3D EF BE AD DE 74 0D 8B 49` | code/mk9.cpp:23,28 |
| SetCharacter | `0x008141A0` | overrides char selection for P1/P2 (unk=1 for tag partner) | `__cdecl` void(*)(PLAYER_NUM, int isBackup, char* name) | **MATCH** - STRONG prologue, CC-pad, 6 direct E8 callers; conv: CONSISTENT | `55 8B EC 83 EC 4C A1 00 8F DF 00 33 C5 89 45 FC` | code/mk9.cpp:33 |
| SetStage | `0x00811DD0` | overrides stage name | `__cdecl` void(*)(const char* stage) | **MATCH** - STRONG prologue, CC-pad, 3 direct E8 callers; conv: CONSISTENT | `55 8B EC 81 EC 8C 00 00 00 A1 00 8F DF 00 33 C5` | code/mk9.cpp:38 |
| GetAIDrone (info->drone) | `0x0044D480` | returns AI drone object for a PlayerInfo | `__thiscall` int(*)(int info) | **MATCH** - PLAUSIBLE prologue, CC-pad, 39 direct E8 callers; conv: CONSISTENT | `56 8B B1 AC 0B 00 00 85 F6 74 34 81 FE EF BE AD` | code/mk9.cpp:82 |
| SetAIScript (drone,script) | `0x00891A80` | assigns an AI script to a drone | `__cdecl` void(*)(int drone, const char* script) | **MATCH** - STRONG prologue, CC-pad, 2 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 4D 08 85 C9 74 3F F6 81 5C 09 00 00` | code/mk9.cpp:84 |
| RunScript (MKScript fn on char script ctx) | `0x0045C610` | executes MK script function on a character (script VM!) | `__thiscall` void(*)(int ctx /*obj+0x2FA4*/, MKScript*, int fnId) | **MATCH** - STRONG prologue, CC-pad, 25 direct E8 callers; conv: CONSISTENT | `55 8B EC 53 8B 5D 08 56 8B F1 57 39 35 F4 9D E3` | code/mk9.cpp:104 |
| AddKoins | `0x00972A70` | adds Koins to profile | `__cdecl` void(*)(int info, int amount) | **MATCH** - STRONG prologue, CC-pad, 4 direct E8 callers; conv: CONSISTENT | `55 8B EC 53 8B 5D 08 85 DB 74 7D 57 8B BB 40 0B` | code/mk9.cpp:194 |
| VersusScreen sprintf-like | `0x00616500` | original callee of hooked call at 0x8146A3 | `__cdecl` void(*)(int len, char* buf, char* fmt, char* charName, char* tex) | **MATCH** - STRONG prologue, CC-pad, 102 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 55 08 8D 45 14 50 8B 45 0C 8D 4D 10` | code/mk9.cpp:322 |
| MKCamera::SetPosition | `0x0045E1C0` | original camera position setter; 29 call sites hooked | `__thiscall` void(*)(MKCamera*, FVector*) | **MATCH** - STRONG prologue, CC-pad, 29 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 45 08 85 C0 74 28 D9 00 50 D9 99 0C` | code/mkcamera.cpp:11 |
| MKCamera::SetRotation | `0x0045DB30` | original camera rotation setter; 31 call sites hooked | `__thiscall` void(*)(MKCamera*, FRotator*) | **MATCH** - STRONG prologue, CC-pad, 31 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 45 08 85 C0 74 23 F3 0F 7E 00 66 0F` | code/mkcamera.cpp:17 |
| MKCamera::SetFOV | `0x0045DAE0` | original FOV setter; 17 call sites hooked | `__thiscall` void(*)(MKCamera*, float FOV, int) | **MATCH** - STRONG prologue, after-RET, 17 direct E8 callers; conv: CONSISTENT | `55 8B EC 83 7D 0C 00 F3 0F 10 45 08 F3 0F 11 81` | code/mkcamera.cpp:23 |
| MKScript::GetFunctionID | `0x00458680` | script function lookup by name/hash | `__thiscall` int(*)(MKScript*, char* name, int) | **MATCH** - STRONG prologue, CC-pad, 152 direct E8 callers; conv: CONSISTENT | `55 8B EC 53 8B 59 20 56 33 F6 57 85 DB 76 42 8B` | code/GameInfo.cpp:5 |
| LoadScriptPackage | `0x00611E80` | load/get script package | `__cdecl` int(*)(int, const char* pkg) | **MATCH** - STRONG prologue, CC-pad, 41 direct E8 callers; conv: CONSISTENT | `55 8B EC 6A FF 68 50 9B C0 00 64 A1 00 00 00 00` | code/GameInfo.cpp:10 |
| GetScript(package,name) | `0x0048F850` | get script from package | `__cdecl` MKScript*(*)(int pkg, const char* name, int) | **MATCH** - STRONG prologue, CC-pad, 47 direct E8 callers; conv: CONSISTENT | `55 8B EC 6A FF 68 88 76 BF 00 64 A1 00 00 00 00` | code/GameInfo.cpp:14 |
| FName::FName ctor | `0x00600980` | UE3 FName constructor (name->index) | `__thiscall` void(*)(FName*, const char*, EFindName, int) | **MATCH** - STRONG prologue, CC-pad, 1425 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 45 0C 56 6A 01 50 8B F1 8B 4D 08 6A` | code/unreal/FName.cpp:5 |
| USkeletalMeshComponent::GetBoneLocation | `0x0077C3B0` | bone world location | `__thiscall` void(*)(USkelMesh*, FVector*, int boneIdx, int, int) | **MATCH** - PLAUSIBLE prologue, CC-pad, 4 direct E8 callers; conv: CONSISTENT | `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04` | code/unreal/SkeletalMeshComponent.cpp:11 |
| USkeletalMeshComponent::GetBoneQuaternion | `0x0077C2B0` | bone quaternion | `__thiscall` void(*)(USkelMesh*, FQuat*, int boneIdx, int, int) | **MATCH** - PLAUSIBLE prologue, CC-pad, 1 direct E8 callers; conv: CONSISTENT | `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04` | SkeletalMeshComponent.cpp:17 |
| USkeletalMeshComponent::GetBoneMatrix | `0x0076E250` | bone matrix | `__thiscall` void(*)(USkelMesh*, FMatrix*, int id, int space) | **MATCH** - STRONG prologue, CC-pad, 35 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 91 E4 03 00 00 8B 45 0C 56 57 85 D2` | SkeletalMeshComponent.cpp:22 |
| SkeletalMesh::MatchRefBone (FindBoneIndex) | `0x00779270` | bone-name->id lookup | `__thiscall` int(*)(int skeleton /*this+640*/, int* ids) | **MATCH** - STRONG prologue, CC-pad, 65 direct E8 callers; conv: CONSISTENT | `55 8B EC 8B 55 08 53 8B 1A 56 57 33 C0 83 CF FF` | SkeletalMeshComponent.cpp:38 |
| Dismember step 1 | `0x0077BD70` | dismemberment prep | `__thiscall` void(*)(USkelMesh*) | **MATCH** - STRONG prologue, CC-pad, 1 direct E8 callers; conv: CONSISTENT | `55 8B EC 83 EC 0C 56 8B F1 8B 86 80 02 00 00 85` | SkeletalMeshComponent.cpp:45 |
| Dismember step 2 | `0x00766920` | dismember part | `__thiscall` void(*)(USkelMesh*, int partID) | **MATCH** - STRONG prologue, CC-pad, 1 direct E8 callers; conv: CONSISTENT | `55 8B EC 56 8B F1 33 D2 39 96 80 05 00 00 0F 8E` | SkeletalMeshComponent.cpp:46 |
| FMatrix from quat/origin (commented out) | `0x005ED0E0` | only in a comment | `__thiscall` FMatrix*(*)(FMatrix*, FQuat*, FVector*) | **MATCH** - STRONG prologue, CC-pad, 16 direct E8 callers; conv: CONSISTENT | `55 8B EC 83 EC 08 F3 0F 10 15 94 0C C6 00 8B C1` | SkeletalMeshComponent.cpp:49 |

### 3.3 Patch sites (InjectHook)

| Symbol | MK9Hook addr | Purpose | CallConv | Verification result | bytes (16) | Source |
|---|---|---|---|---|---|---|
| Versus screen text call site | `0x008146A3` (`E8` call patch) | redirect to HookLoadPlayerVersusScreen (then calls 0x616500) | __cdecl | **MATCH** - `E8 58 1E E0 FF` -> `0x00616500` == expected | `E8 58 1E E0 FF 8B C7 83 C4 20 8D 48 01 8A 10 40` | dllmain.cpp:41 |
| SetupFight call site | `0x0080DF82` (`E8` call patch) | redirect to SetupFight() each time a fight starts; MK9Hook hooks the FIGHT-START event here | __cdecl (void,void) | **MATCH** - `E8 49 05 0F 00` -> `0x008FE4D0` (not named by MK9Hook); target bytes `55 8B EC A1 EC 66 ED 00 85 45 08 74 10 8B 55 0C`, 1703 callers | `E8 49 05 0F 00 83 C4 08 57 68 00 80 00 00 8B CE` | dllmain.cpp:42 |
| XInputGetState wrapper (PATCH_JUMP) | `0x0099CA36` (`E9` jmp patch, 5 bytes) | overwrites first 5 bytes with E9 -> XInputGetState_Hook; zeroes Gamepad while menu open | __stdcall (DWORD, XINPUT_STATE*) | **MATCH** - existing bytes `FF 25 20 58 C5 00` = import thunk `jmp [0xC55820]` -> XINPUT1_3 ordinal #2 (XInputGetState); 2 callers: 0x422E50 and 0x424B01 (inside sampler 0x424A50) | `FF 25 20 58 C5 00 FF 25 1C 58 C5 00 CC CC CC CC` | dllmain.cpp:127 |

Camera call-site families (all `InjectHook(addr, &MKCamera::HookedXxx, PATCH_CALL)`, dllmain.cpp:44-123). The hook member functions are `__thiscall` wrappers that run MK9Hook logic and then call the original (`SetPosition`/`SetRotation`/`SetFOV`):

| Family | Sites | Original target | Result | Site VAs (0x00xxxxxx) |
|---|---|---|---|---|
| MKCamera::SetPosition call sites | 29 | `0x0045E1C0` | 29/29 are `E8` calls landing exactly on the expected target | 82A24D 86AAB7 86CC89 86D13C 86D1E7 86D2B6 86D6E8 86D8E0 86DDB4 873462 8743C3 875F2E 878A46 878FB4 879229 87B3F6 87B73D 87B872 87B8D7 87C756 87CA63 87D382 87D4E2 87D5C7 87D8C1 87DC44 87E42C 8DB93A 9782E2 |
| MKCamera::SetRotation call sites | 31 | `0x0045DB30` | 31/31 are `E8` calls landing exactly on the expected target | 82A25B 86AAC7 86CC94 86D144 86D2C1 86D6F3 86D8EB 86DDBF 86EF00 86EF15 86EF2A 873470 876044 8760A1 8760FE 877B70 878A88 878FC2 87921E 87B401 87B732 87B87D 87B8E2 87CA6E 87D4ED 87D5D2 87D8C9 87DC4C 87E437 8DB96A 9782F0 |
| MKCamera::SetFOV call sites | 17 | `0x0045DAE0` | 17/17 are `E8` calls landing exactly on the expected target | 82A26E 82A2E9 86B5A3 86B5C6 86D34A 86DE3F 8761F9 876214 876264 87629C 8795D9 87B88D 87B8F2 87C6BF 87E44C 8DB8D1 978303 |

Camera object layout confirmed from the original setters' bytes: `SetPosition` stores 3 floats at `camera+0x10C/0x110/0x114`; `SetRotation` stores a 12-byte FRotator (Pitch,Yaw,Roll ints) at `camera+0x118` (MK9Hook's `GetMatrix` reads `this+280` = 0x118 **match**); `SetFOV(float, int)` stores the float at `camera+0x140`.

## 4. Struct offsets and known structures

Method: whole-.text census of `[reg+disp32]` operands equal to the offset (supportive only - small offsets are ubiquitous) plus hand-decoded bodies where available.

| Structure.field | Offset | MK9Hook use | Verification |
|---|---|---|---|
| **PlayerInfo array** | base `0xE89D7C`, stride `0xC8C`, 4 records, index 0-based (`PLAYER1=0 ... PLAYER4=3`) | `GetInfo(plr)` | **CONFIRMED by bytes of 0x80E540**: `cmp [ebp+8],4; jl L; mov eax,0xE89D7C; ret 4; L: imul eax,eax,0xC8C; add eax,0xE89D7C; ret 4`. ECX ignored. Index >=4 silently returns record 0. 0xE89D7C is referenced from 13 code sites. = `FG_GAME_INFO(0xE89B70) + 0x20C`. |
| PlayerInfo+0x1C / +0x20 | handle pointer + id | via GetObj(info,0) | **CONFIRMED** (0x895B70): `p=[ecx+0x1C]; if(!p||p==0xDEADBEEF) return 0; id=[ecx+0x20]; if(id==-1) return 0; return ([p+4]==id)?p:0; ret 4`. Weak-pointer/handle dereference; matches prior combat-state-investigation.md. Called from 278 sites. |
| PlayerInfo+0xEC (236) | embedded second handle block ("backup"/tag partner) | `GetBackupInfo = info+236` then GetObj | UNVERIFIED semantics (901 `[reg+0xEC]` accesses, too common). If true, the partner handle pair is at PlayerInfo+0x108/+0x10C. |
| PlayerInfo+0xBAC / +0xBB0 | AI drone handle (ptr,id) | `0x44D480(info)` | **CONFIRMED** by body of 0x44D480 (`mov esi,[ecx+0xBAC]`, sentinel 0xDEADBEEF, id at +0xBB0). |
| PlayerInfo+0xB40 | gamepad pointer (from prior feasibility.md; not used by MK9Hook) | - | prior doc only |
| PlayerInfo+0xC14 | super meter float (0..1) | `SetCharacterMeter` | PLAUSIBLE: 48 `[reg+0xC14]` accesses, sample 0x830282 `movss xmm0,[eax+0xC14]`. |
| PlayerInfo+0x964 (2404) | character name char[] | `GetCharacterName` | **CONFIRMED-ish**: `lea esi,[ebx+0x964]` at 0x814292 inside SetCharacter (0x8141A0) feeding a copy; 19 accesses. |
| PlayerInfo+0x17EC | damage (combo) scale float | `SetCharacterDamageScale` | **NOT VERIFIED / SUSPICIOUS**: zero occurrences of 0x17EC as any 4-byte immediate in code, and 0x17EC > stride 0xC8C, so for P1 it aliases PlayerInfo[1]+0xB60. Do not rely on it. |
| Obj (resolved by GetObj) +0x08 | Actor* | `*(obj+8)` | UNVERIFIED (disp8). |
| Obj+0xF4 | per-object time scale float | `SetCharacterSpeed` | **CONFIRMED semantics**: virtual `GetTimeScale()` at 0x8949E0 = `fld [ecx+0xF4]; fmul [0xE91E24]; ret` (vtable slot at 0xD6B3E8). |
| Obj+0x10C (268) | USkeletalMeshComponent* | `GetSkeleton` | UNVERIFIED (1238 accesses; ubiquitous). Note camera also has data at +0x10C (different class). |
| Obj+0x2FA4 | script-VM context (ECX of RunScript 0x45C610) | `RunCharacterScript` | PLAUSIBLE (33 accesses, sample 0x44F0BA/0x44F65D/0x450F56 in the 0x44xxxx-0x45xxxx script area next to RunScript). |
| Obj+0x78C0 | life float (0..1) | `SetCharacterLife` | PLAUSIBLE: 74 accesses; 0x810100 writes it then mirrors to +0x78C4 (`fld [eax+0x78C0]; fstp [eax+0x78C4]`) - +0x78C4 is a companion value (likely lagging/display health). |
| Actor+0xDC | FVector location (3 floats) | `GetCharacterPosition` | UNVERIFIED (823 accesses). |
| Actor+0x11C (0x118+4) | FVector scale | `SetCharacterScale` | UNVERIFIED (563 accesses). |
| USkeletalMeshComponent+0x280 | reference-skeleton pointer | bone-id lookup | **CONFIRMED-ish**: Dismember-step-1 (0x77BD70) opens with `mov eax,[esi+0x280]`. |
| MKCamera+0x10C/0x110/0x114, +0x118, +0x140 | pos / FRotator / FOV | camera hooks | **CONFIRMED** from original setter bytes (see 3.3). |
| `FG_GAME_INFO` @ 0xE89B70 | static singleton object | GFG_GAME_INFO | **CONFIRMED** as an object (482 `mov ecx,0xE89B70`), name from log string "FG_GAME_INFO::GameInit()" at 0xD5DBA0. Flags dword at +0x10 (0xE89B80) is cleared of bit 0x200000 in GameInit. |
| MKScript / package | opaque | GetScript(pkg,name) | functions verified (3.2); structures unknown. |
| UE3 types FName/FVector/FRotator/FQuat/FMatrix | - | helpers | FName ctor 0x600980 (__thiscall, 3 args, RET 12) MATCH, 1425 callers. |

Not present in MK9Hook (searched): RNG addresses, frame/tick counters, input button bitmasks, pad structs (it only zeroes XINPUT_GAMEPAD while its menu is open), DirectInput, network, save-state.

## 5. Cross-check against prior independent findings

(From `analysis/feasibility.md` and `analysis/combat-state-investigation.md`; bytes re-read from the exe in this pass.)

| Prior item | Result in this pass |
|---|---|
| `0x0080E540` player-info accessor, records `0xE89D7C` stride `0xC8C` | **COINCIDES - same function as MK9Hook `GetInfo`.** New facts: ECX is unused (MK9Hook's `*(int*)0xE89B70` first-dword is irrelevant); argument is a 0-based index with bounds clamp to record 0; `ret 4` (thiscall/stdcall-style pop). 219 direct callers. |
| `0x00895B70` handle validator (obj+0x1C, +0x20 vs pointee +4) | **COINCIDES - MK9Hook `GetObj`/`GetBackupObj`.** Bytes decoded; `ret 4`; 278 callers. |
| `0x00424A50` hardware input sampler | MK9Hook has no symbol here, but its `XInputGetState` thunk patch (0x99CA36) is called at **0x424B01 inside the sampler** (`push &pad_state(esi+0xE0E34C); push index; call 0x99CA36`) and at 0x422E50. 0x424A50 has a single caller 0x404E26. No contradiction. |
| `0x00404D30` main loop | Not touched by MK9Hook. Single caller 0x404F09; it calls sampler (0x404E26) and 0x4025A0 (0x404E35). Confirms prior ordering sampler -> timed engine. |
| `0x004025A0` timed engine phases | Not used by MK9Hook (single caller 0x404E35). |
| `0x0080F670` (C0) / `0x0080DD40` (BC) engine vtable slots | Not used by MK9Hook. Verified in vtable at 0xD60D30: +0xBC -> 0x80DD40 (@0xD60DEC), +0xC0 -> 0x80F670 (@0xD60DF0). MK9Hook's fight-start hook site (0x80DF82 in `FG_GAME_INFO::GameInit` 0x80DF50) and GetInfo (0x80E540) lie in the same 0x80D*-0x80F* code cluster, i.e. the game-flow/engine class. RNG generator `0x5F5080` is called from **0x80F6C0 inside C0** (consistent with prior "C0 consumes RNG"). |
| RNG `0x005F5080`/`0x005F50A0`, seed `0x005ED570`, state `0x00E7C054` | Not in MK9Hook. Bytes: `0x5F5080: eax=[0xE7C054]*0x0BB38435+0x3619636B; store; and 0x7FFFFFFF`. **Confirms the prior LCG constants.** State VA has exactly 5 code refs (the two generators + seed setter 0x5ED58D). Callers of int RNG: 0x49205D, 0x493FDD, 0x494BF2, 0x495B28, 0x80F6C0; float RNG: 0x491A4D; seed setter: 0x81552A and 0x884830 (network input startup). |
| VA `0x00E43054` ("pause-like flag", getter source) | **COINCIDES with MK9Hook `ms_bPauseGame` (mk9menu.cpp:186)** - MK9Hook declares it but never reads/writes it. Getter 0x406790 byte-verified (`mov eax,[0xE43054]; ret`, 35 callers). New: setter `0x407AB0` (writes 0xE43054 and 0xE43058), 11 call sites; other direct refs 0x41E246, 0x799302, 0x7AEEA9, 0x7AEF43, 0x82A950, 0x904305, 0x9043F1. It is an int32, not a bool. |
| `0x00E53BFC`, `0x00E3B85C`, `0x00E5873C`, `0x00E7C010` | Not in MK9Hook; all exist with code refs (26 / 6 / 407 / 3). |
| MK9Hook-only: `FG_GAME_INFO` singleton `0xE89B70`; global time-scale `0xE91E24`; `GameInit` 0x80DF50; `SetPause` 0x407AB0; drone handle +0xBAC | New facts not in prior docs. |
| Contradictions found | None with prior docs. MK9Hook-internal inconsistencies: (a) `*(int*)GFG_GAME_INFO` treats the singleton as a pointer; (b) damage-scale offset 0x17EC exceeds record stride; (c) `bool` for a dword flag. |

## 6. Directly relevant to rollback (tick / speed / pause / input / player state / RNG)

**Verdict on MK9Hook's scope:** it has no freeze, pause, frame-advance, fixed-step or RNG code. Its only time features are *slow motion* (global float `0xE91E24`, menu range 0.0-2.0, F5 toggles, default 0.5) and a per-character speed (`Obj+0xF4`, 0-10). Both are consumed by one virtual function, so they scale **per-object animation/movement time**, not the engine clock.

1. **Game speed / time scale (verified).** `0x8949E0` (virtual, vtable slot @0xD6B3E8, no direct callers) returns `Obj+0xF4 * *(float*)0xE91E24`. The global has exactly one reader. The global is BSS; the explicit store that sets it to 1.0 was not found statically (confirm by a dynamic read; expected 1.0 in normal play). For rollback: pin `0xE91E24 == 1.0` and `Obj+0xF4` consistent on both peers, and include both in the snapshot or assert them. MK9Hook writes the global from its own 1 ms thread without synchronisation (races the game thread) - do not copy that.
2. **Pause (verified, unused by MK9Hook).** `0xE43054` (+ `0xE43058`) with getter `0x406790`, setter `0x407AB0` (cdecl-style, 1 arg, 11 callers). Prior doc: manager `+0x70` only advances while this is clear. This is the natural pause/freeze lever for local frame stepping/stall during rollback re-simulation (needs a behavioural test; MK9Hook gives no evidence of behaviour, only the VA).
3. **Tick.** MK9Hook has none. It runs its own `Sleep(1)` thread (HookUpdate) writing cheats continuously - asynchronous to the game's loop. Our verified anchors for a deterministic hook point are the **call sites inside the main loop**: `0x404E26` (call sampler 0x424A50) and `0x404E35` (call 0x4025A0), inside `0x404D30` (caller 0x404F09). An `InjectHook`-style E8 retarget at those two sites is the MK9Hook-equivalent technique and is safe to emulate.
4. **Fight start event.** `0x80DF82` is the `E8` call to the logger `0x8FE4D0(0x10, "FG_GAME_INFO::GameInit()")` inside `FG_GAME_INFO::GameInit` (0x80DF50). MK9Hook replaces that log call with its `SetupFight` (the original log call is dropped - cdecl, 2 stack args cleaned by the caller's `add esp,8`, so a no-arg replacement is stack-safe). Good session-reset/"new match" signal; round starts are not covered.
5. **Input.** Only the `XInputGetState` import thunk `0x99CA36` (`jmp [0xC55820]`, ordinal #2; neighbour 0x99CA3C = ordinal #3 XInputSetState). It has exactly 2 call sites: sampler 0x424B01 and 0x422E50. This is a single funnel for XInput data and a clean place to *inject remote/predicted input* for pad slots, and MK9Hook proves patching the thunk works. Keyboard and DirectInput paths are separate (prior docs) and MK9Hook does not touch them. MK9Hook zeroes `pState->Gamepad` while its menu is open (leaves dwPacketNumber alone). Pad state buffer lives at `esi+0xE0E34C` in the sampler object.
6. **Player state.** `PlayerInfo[4]` is a *static* array: VA `0xE89D7C + i*0xC8C` (0x3230 bytes total) - snapshot/restore candidate with no pointer chasing for the record itself; contains handle pairs (+0x1C/+0x20 main pawn, +0xBAC/+0xBB0 AI drone, +0xEC.. partner), meter (+0xC14), name (+0x964), pad pointer (+0xB40). The live pawn is reached via `GetObj`: `Obj+0x78C0` life (+0x78C4 companion), `Obj+0xF4` time scale, `Obj+0x08` Actor with location at `Actor+0xDC`, `Obj+0x2FA4` script-VM context. Handles carry an id compared with `[ptr+4]`, so snapshots must canonicalise handles, not raw pointers (consistent with prior notes). Combo/damage scale is NOT reliably at PlayerInfo+0x17EC (see section 4).
7. **Script VM.** `GetScript(pkg,name)` 0x48F850 (cdecl), package loader 0x611E80 (cdecl), `MKScript::GetFunctionID` 0x458680 (thiscall), `RunScript` 0x45C610 (thiscall, ECX = `Obj+0x2FA4`, args MKScript*, fn id; RET 8). The fighters' move logic runs in this VM, so script contexts are part of the combat state to snapshot, and `RunScript` is a lever for calling script functions deterministically.
8. **RNG.** Not in MK9Hook. Independently re-verified in our exe: LCG state `0xE7C054`, int `0x5F5080`, float `0x5F50A0`, seed `0x5ED570`; constants `0x0BB38435`/`0x3619636B`; output masked to 31 bits.
9. **Misc useful signals.** `AddKoins` 0x972A70 (cdecl) opens with `mov edi,[ebx+0xB40]` on its PlayerInfo argument - the same +0xB40 field prior docs call the primary gamepad pointer, so it is more likely a player-controller/profile object that owns the pad; `SetCharacter` 0x8141A0 / `SetStage` 0x811DD0 (cdecl) are the character/stage selection entry points used by the fight-start flow (callers 0x8143A3..0x814416, 0x8BB5B4 / 0x8143D7, 0x81563C, 0x8BB53C) - useful for forcing identical match setup on both peers.

## 7. Hooking / overlay architecture worth emulating

**Initialisation flow.** Loaded as `.asi` by Ultimate ASI Loader acting as a **`dinput8.dll` proxy** (our exe imports `DINPUT8.dll!DirectInput8Create`, so this loads very early). `DllMain(DLL_PROCESS_ATTACH)`: (1) `CheckGame()` string gate at 0xD15624; (2) load ini (`SettingsMgr->Init`); (3) `OnInitializeHook()` - patches all call sites **synchronously inside DllMain** (the proxy is loaded at import time, so this normally precedes the game's own startup code); (4) `DisableThreadLibraryCalls`; (5) `CreateThread(DirectXHookThread)` and `CreateThread(HookUpdate)`. `DLL_PROCESS_DETACH` only calls `kiero::shutdown()`.

**Hooking primitives.** Two layers: (a) *Static code patches* via ModUtils-style `MemoryMgr.h` `Memory::VP::InjectHook(addr, target, PATCH_CALL|PATCH_JUMP)`: `VirtualProtect(addr,5,PAGE_EXECUTE_READWRITE)`, write opcode `E8`/`E9`, write rel32 = target - (addr+5), restore protection; **no FlushInstructionCache** (add it); member-function pointers are converted via union bit-copy. Because MKKE has no ASLR, absolute VAs are stable. (b) *Vtable hooks for D3D9* through **kiero + MinHook** (`MH_CreateHook`+`MH_EnableHook`, hde32 length disassembler). MinHook is *not* used for the game functions - all game hooks are retargeted E8 calls, which is cheaper and gives the original back as a plain function.

**Calling game code.** Raw function-pointer casts with the right convention (`__thiscall` casts rely on MSVC generating ECX), no wrappers/trampolines.

**Update thread.** `HookUpdate`: `while(true){ HookProcessStuff(); UpdateMouse(); Sleep(1); }` - runs concurrently with the game loop; it polls `GetAsyncKeyState` (free-cam, script hotkeys) and writes game memory unsynchronised. For rollback, replace with callbacks at the verified main-loop call sites (404E26/404E35) so state reads/writes occur at tick boundaries.

**DirectX overlay.** `DirectXHookThread`: `Sleep(2000)`, then loops `kiero::init(D3D9)` until success (no sleep between retries - the game loads d3d9.dll late; our exe holds the strings "d3d9.dll", "d3dx9_43.dll", "Direct3DCreate9Ex" but does not import them). `kiero` registers a 100x100 dummy window class "Kiero", calls `Direct3DCreate9` + `CreateDevice(HAL)`, copies **119** vtable entries, then `kiero::bind(42, EndScene)` and `bind(16, Reset)` (MinHook on the shared d3d9.dll code). In the first `EndScene` call: `pDevice->GetCreationParameters` -> `hFocusWindow`; subclass WndProc via `SetWindowLongPtr(GWL_WNDPROC)`; `ImGui::CreateContext`, `ImGui_ImplWin32_Init`, `ImGui_ImplDX9_Init`; config flags `NoMouseCursorChange | NavEnableGamepad | NavEnableKeyboard`; every frame: `ImGui_ImplDX9_NewFrame, ImGui_ImplWin32_NewFrame, ImGui::NewFrame`, draw notifications/menu, `EndFrame, Render, ImGui_ImplDX9_RenderDrawData`, then the original EndScene. `Reset` hook: `InvalidateDeviceObjects` -> original `Reset` is called by returning `m_pReset` after `CreateDeviceObjects` (note: its order is invalidate, create, then original - the usual pattern is invalidate, original, create; flag when emulating). Dear ImGui **1.85 WIP** (IMGUI_VERSION_NUM 18408), Win32+DX9 backends. WndProc: `WM_KILLFOCUS/SETFOCUS` focus flag; `WM_KEYDOWN` of the configured key (default **F1**) toggles the menu; **F5** slow-mo toggle; **F2** HUD toggle; while the menu is active input is routed to ImGui and swallowed (`return true` to the game). Gamepad: separate thread maps XInput to ImGui nav and both-sticks toggles menu; `XInputGetState_Hook` zeroes the pad while the menu is open. Settings: `MK9Hook.ini` + `mk9hook_user.ini` (key binds, `fMenuScale`, mouse), custom IniReader.

**What to emulate for MK9Rollback.** (1) Proxy-DLL/ASI injection + synchronous `InjectHook` of E8 sites at load (we verified the sites that matter, 404E26/404E35 etc.). (2) kiero+MinHook EndScene/Reset overlay for a netcode HUD (ping, rollback frames, desync) - add retry-with-sleep, and guard that the hooked EndScene belongs to the game device. (3) Keep a thin version gate (exe SHA-256, not a string). (4) Avoid: free-running 1 ms writer thread, unsynchronised cheat writes, missing FlushInstructionCache, and treating `0xE89B70` as a pointer.

## 8. Open items / suggested next steps

1. Dynamic read of `0xE91E24` (expect 1.0) and who initialises it; read `0xE43054/58` during pause menu to confirm semantics; flip `0xE43054` in a scratch session to test freeze behaviour.
2. Confirm PlayerInfo[+0x1C/+0x20, +0xEC.., +0xBAC, +0xC14, +0x964] live (read-only probe) and test whether the damage-scale really lives at `PlayerInfo+0x17EC` of the *other* player (aliasing theory) or elsewhere.
3. Disassemble/inspect the virtual that owns `GetTimeScale` (vtable at ~0xD6B3C8) to see which object class carries `+0xF4` and where delta is multiplied.
4. Identify callers of the logger-based GameInit site's real owner (0x80DF50 has no direct caller / vtable ref; maybe reached through thunks at 0x80DF30/0x80DF40) to get a proper function-level fight-start hook instead of a log-call site.
5. Check the exe-side `Direct3DCreate9Ex` usage versus kiero's non-Ex dummy device before relying on the EndScene slot hook (MK9Hook works in the field, but we have not run it).
