#pragma once
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

// Gate D groundwork: save/restore of an explicit list of memory spans.
// A span is restored only if its object identity is unchanged (base address and,
// for handle-validated objects, generation id), and dwords marked "keep" (live
// pointers into heap objects whose lifetime we do not own) are never written back.
// Nothing here is used for gameplay until the restore experiments pass.
namespace Snapshot {

struct Span {
    const char* name;
    uintptr_t base;
    uint32_t size;
    uint32_t identity;               // e.g. generation id read at save time; 0 = none
    std::vector<uint32_t> keepOffsets;  // dword offsets never restored
};

struct State {
    uint32_t gameFrame = 0;
    std::vector<Span> spans;
    std::vector<std::vector<uint8_t>> bytes;
};

// Build the current candidate span list (fighters, PlayerInfo, pads, RNG, GameInfo head).
std::vector<Span> CandidateSpans();

void Save(State* out, uint32_t gameFrame);
// Returns number of spans restored; spans whose identity changed are skipped and logged.
int Load(const State& s);

// Tier A (static globals, raw) + Tier B (MK object heap image) + fighters' actors.
// v1 rules (docs/state_ownership.md §5): refuse when the script VM is not idle,
// and refuse a load if any MK object was created since the save (unless forced).
struct StateAB : State {
    uint32_t idCounter = 0;
    std::vector<uint32_t> bindRec[2];  // fighter SkelControl binding state, restored atomically at load (BindState)
    std::vector<uint32_t> mapObjs;     // MK skeletal objects whose +0x138 name map was saved: {object, MK object id} pairs
};
extern bool gReflectData;  // UE3 spans: fields declared as data (reflection) are never kept live by the merge
// LoadAB's pointer-safe merge (rc25): 0 whole-span (before rc25), 1 diff-first (default: only the dwords whose live and
// saved values differ are classified; the same bytes), 2 diff-first checked against the whole-span merge on every span
// (DIFFMERGE MISMATCH lines, the whole-span result used). Not wired to a command yet ('diffmerge N').
extern int gDiffMerge;
extern bool gPinFloats;  // SkelControlPinLimb +0x90..+0x98/+0xA8..+0xB0 always restored by value
extern bool gCompFloats;  // fighter component float vectors (COMPKEEP) always restored by value ('abtest compfloats')
extern bool gCompLiveData;  // fighter component kept dwords whose live value is data restored by value ('abtest complivedata')
extern bool gListRescue;  // fighter comp +0x3A8 list freed after the save: restore it from the quarantine
extern bool gBindState;      // save the fighters' SkelControl binding state ('bindstate')
extern uint32_t gFxActorSaved, gMkActorSaved, gMkBaseSaved;
extern bool gPadFullLoad;  // AI/remote pad restored as the whole 0x60C object ('abtest padfull')
extern bool gPadHwLive;    // hardware pad slots keep their device bits (+8: 0x1 attached, 0x20 kind) live at loads ('abtest padhw')
extern bool gFxActors, gFxActorsLoad;  // fighters' attached actors saved / restored ('abtest fxactors')
extern bool gMoveReqs, gMoveReqLoad;  // fighters' move-request entries (fighter+0x7524) saved / restored ('abtest movereq')
constexpr uint32_t kMoveReqSize = 0x248;
void LogFight(uint32_t gf);  // 'FIGHT at gf=..: P1 <mesh>, P2 <mesh>' (which characters)
// 'cinewatch 1' (lab): every live fight frame, the UE3 objects whose class name contains "Cinema" (births, deaths and
// the 64-byte chunks of their bodies that changed) and the fighters' motion-control slots and cinematic fields, to
// find what an X-ray's cinematic changes that a load must restore. Frames log, "CINE" lines.
extern bool gCineWatch;
void CineWatch(uint32_t gf, const uint32_t actors[2]);
extern bool gCostDmg, gCostDmgLoad;  // fighters' costume damage pieces (comp +0x570 groups) ('abtest costdmg')
extern bool gMkActors, gMkActorsLoad;  // other MK objects' actors (obj+0x08) saved / restored ('abtest mkactors')
extern bool gMkBases;  // the Base actors those follow (actor +0x60), saved as mkactor spans when no other span has them ('abtest mkbases')
// The last load's mkactor span for this actor: -1 none, 0 restored (loc = the Location written), 1 block/identity
// changed, 2 bDeleteMe, 3 mkactors load off, 9 skipped after the checks.
int MkActorAtLoad(uint32_t actor, float loc[3]);
int MkActorsRestored(uint32_t* out, int max);  // the actors whose mkactor span the last load wrote
// The last loaded snapshot's saved dword at addr and the name of the span holding it (nullptr: in no span).
const char* SavedDword(uint32_t addr, uint32_t* value);
extern bool gMkHelpers, gMkHelpersLoad;  // MK objects' 0x4C helper blocks (obj+0x1D8) saved / restored ('abtest mkhelper')
// Helper frees held by the quarantine, and a helper whose destructor ran in the abandoned future (vtable 0x00C6A5A8)
// restored too ('abtest mkhelperrevive')
extern bool gMkHelperRevive;
// VMTRACE: what the last load did with an MK object's helper (restored, skipped and why, or not in the snapshot).
void DescribeHelper(uint32_t owner, uint32_t helper, char* out, size_t n);
// The MK helper blocks the last load restored (RayQueries re-issues their saved in-flight line checks).
const std::vector<uint32_t>& RestoredHelpers();
extern uint32_t gMkHelperSaved, gMkHelperRestored;
extern bool gListRebase;  // bindstate into a reallocated +0x3A8 list ('abtest listrebase')
extern bool gBindStateLoad;  // restore it at load ('abtest bindstate')
extern bool gBindPins;  // the bind state also covers the leg-IK limb pins (comp +0x390) and their requests (+0x378) ('abtest bindpins')
bool SaveAB(StateAB* out, uint32_t gameFrame, char* why, size_t whyLen);
int LoadAB(const StateAB& s, bool force, char* why, size_t whyLen);
int ExternalBlocks(std::vector<std::pair<uintptr_t, uint32_t>>& out);  // xblock candidates (diagnostics)
bool TierACovers(uint32_t addr, uint32_t size);  // [addr, addr+size) lies inside one Tier A raw range
extern bool gScriptData;  // MKScript context data areas ("scriptdata 0|1")
extern bool gTierEOn;  // Tier E: fighters' UE3 anim-tree objects (experiment, "tiere 0|1")
extern bool gScriptScratch;  // Tier C MKScript scratch buffer (A/B toggle: "scratch 0|1")
extern bool gHeapLock;  // hold the MK heaps' spinlocks during save/load of the image ('heaplock 0|1')
extern bool gFastProbe;  // the save's skel phase checks reads with guarded loads, not VirtualQuery ('fastprobe 0|1')
extern bool gNameMaps;  // fighters' name-keyed lookup cache TMap at obj+0x138 ('namemaps 0|1')
extern bool gCompListRebase;
extern bool gCompList;  // fighter component queued anim-object list +0x3A8 ('complist 0|1')
// After a load: entries of the fighters' +0x3A8 lists that are unreadable, inside 'gone' (cinematic objects collected
// since S) or unbound are removed ('abtest compscrub'). Returns the entries removed.
extern bool gCompListScrub;
extern uint32_t gCompListScrubbed;
int ScrubCompLists(uint32_t S, const std::vector<std::pair<uintptr_t, uint32_t>>& gone);
extern bool gSkelBound;  // capture fighter SkelControls' bound objects (0x60-byte elements, vtable 0xD5A5E0)
extern bool gSkelBoundLoad;  // restore the captured bound objects at load
extern bool gCompWeights;      // capture fighter comp arrays +0x384 (bound objects) and +0x39C (their weights)
extern bool gCompWeightsLoad;  // restore them at load
extern bool gSkelBind;  // ue3anim SkelControls: restore the saved bound object (+0xC0) at load
extern bool gPropSkel, gPropSkelLoad;  // other MK skeletal objects' pose: save / apply at loads ('propskel 0|1', abtest)
extern bool gBoneRotation;  // fightercomp: restore the saved rotation of the triple-buffered bone matrices ('bonerot 0|1')
extern bool gForeignVtables;  // strict rule also accepts vtables of other modules (PhysX objects) ('foreignvt 0|1')
extern bool gStrictVtable;  // pointer rule above 0x30000000: target must start with a real vtable ('strictvt 0|1')
extern bool gColdCheck;     // targets on pages outside the working set kept live unread ('coldcheck 0|1')
extern int gTrimWsMs;       // test aid: empty the working set every N ms at a save ('trimws N', 0 off)
void StopPageWarmer();      // MarkKeep's page-touching thread (MK9R_Unload)
extern int gLearnFields;  // learned per-address pointer/data kind in the pointer merge ('learnfields 0|1')
extern bool gPositiveCache;  // persistent committed-region cache in the pointer scan ('poscache 0|1')
void SetCommittedCap(int n);  // 'heapcache N': that cache's capacity in ranges (rc25 default 1024; 64 = rc24)
extern bool gFloatFields;  // actor Location never kept live by the pointer merge ('floatfields 0|1')
extern bool gPawnComps;  // the fighters' non-mesh components (collision cylinder...) ('pawncomps 0|1')
extern bool gTierECamera;  // Tier E also takes MKCamera / MKArenaConstrainPoint ('tierecam 0|1')
extern bool gTierENodes;  // Tier E also takes the fighters' anim tree nodes ('tierenodes 0|1')
extern bool gCompSpan;  // fighter SkeletalMeshComponent scalars (pointer-safe merge), 'compspan 0|1'
extern bool gSkelArrays;  // Tier C fighter skeletal-mesh bone arrays (A/B toggle: "skel 0|1")
// Diagnostic: GMalloc small-block free blocks that lie inside the snapshot's spans (or broken
// free-list links). Returns hits + broken chains, -1 if the allocator layout is unexpected.
int CheckFreeLists(const StateAB& s, const char* when, int maxLog);

// Diagnostic: watch one small-block bin for a corrupted free link (logs holders once).
bool WatchBin(uint32_t bin, uint32_t gf);
void CtxProbe();  // 'ctxprobe': log where each script context's data range and variable table live
// Diagnostic: write 'start end' lines for every snapshot byte buffer (to filter memory diffs).
void WriteOwnRanges(FILE* f);
// Name of the Tier A span fully containing [addr, addr+len), or nullptr.
const char* CoveredStatic(uintptr_t addr, uint32_t len);
// Hash of the live Tier A+B+C state (HostFrame and live pointer fields masked).
// Returns span count, fills per-span hashes/names up to maxSpans.
int HashAB(uint32_t* perSpan, const char** names, int maxSpans, uint32_t* combined);
// The persistent level's WorldInfo actor (0 outside a level).
uintptr_t WorldInfoActor();

}  // namespace Snapshot
