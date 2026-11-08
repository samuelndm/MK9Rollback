#include "CinemaRestore.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../debug/Ue3Names.h"
#include "../hook/CodePatch.h"
#include "FreeQuarantine.h"
#include "RollbackManager.h"

namespace CinemaRestore {
bool gRestore = true;
bool gLevelVis = true;
bool gLevelReady = true;
bool gKismet = true;
// Off: probe 13 (2026-11-02) took the X-ray's level out at the X-ray-start load and the replay then put the fighters in
// PHYS_Interpolating at S+1, two frames before the original (4 sync errors instead of 1-2). Kept for experiments.
bool gLevelUndo = false;
bool gLevelRedo = false;  // experiment (2026-11-08): the same, at the level's next lookup instead of at the load
bool gPartial = false;
// 'cinemadeadok' (2026-11-06): a declared object field whose saved value already named no live object at the save does
// not block the restore. Putting such a dangling value back is S's own state, which the first pass ran from; blocking
// left the whole Kismet state on the abandoned timeline (net13 ns100_d3 B: SeqAct_Cinema_0 +0xE0 held an MK object
// destroyed before every save from S=100456 on, each load restored nothing, and at S=100552 the replay activated the
// cinema from the abandoned timeline's queue and called through that object, freed and released: eip 0). 65 of ~1,300
// blocked loads in net1-net13 were SeqAct_Cinema +0xE0, 282 CinemaGroupInstSpawnedEmitter +0x3C, 76 SeqVar_Object +0x8C.
bool gDeadOk = true;
// 'cinemadeadrefs' (2026-11-07, batch 20: 3 crashes in 27 game-fights under network gaps and online X-rays, all on
// Graveyard's looping stage cinematic). A saved object array element (SeqAct Targets +0xE0, the effect actors the
// cinematic drives) or object field naming an object that is no longer a live UObject (an EmitterSpawnable that
// destroyed itself and was purged) blocked the whole restore ("nothing restored"); runs of such loads left the Kismet
// state on the abandoned timeline until a replay ran the cinematic's target loop (0x00414694: [data+4i]->vt+0x38) on
// the purged emitter after the quarantine had released it (eip 65636E61 / FFFFFFFF), or emptied a Targets array whose
// block had been freed before the save (0x004146F9 Realloc of a released block). Now: such array elements are set to 0
// (what the collector does to references to the objects it destroys; that loop skips 0) and such fields keep their
// live value (0 when that is dead too); an array whose restored block was already freed before S keeps its live
// header. Only objects collected since the save ("now collected") still block the restore.
bool gDeadRefs = true;
}

namespace {
constexpr int kRing = 12;                                               // RollbackManager's snapshot ring (slot = gf % kRing)
constexpr uintptr_t kObjects = 0x00E7C0EC, kObjectCount = 0x00E7C0F0;  // GObjObjects data / count
constexpr uintptr_t kCallSite = 0x004BB852;     // 0x004BB820 (CinemaGroupInstSpawned* vtable slot 53): GWorld->DestroyActor
constexpr uintptr_t kDestroyActor = 0x0079D960;  // UWorld::DestroyActor(actor, bNetForce, bShouldModifyLevel), ret 0xC
constexpr uintptr_t kGWorld = 0x00E86AB0;
constexpr uint32_t kObjectMax = 0x1000;  // bytes saved per object at most (the largest seen: SeqAct_Cinema, 624)
constexpr uint32_t kArrayMax = 0x1000;   // TArray blocks saved up to this size
// Spawned-actor groups (0x004BB820): +0x3C the actor, +0x88 bit 0 = the group owns (spawned) it. The teardown clears
// actor +0x80 bits 0x1 and 0x10 before DestroyActor (0x004BB833/0x004BB83D).
constexpr uint32_t kGroupActor = 0x3C, kGroupFlags = 0x88, kActorFlags = 0x80, kTeardownBits = 0x11, kDeleteMe = 0x20;
// Interpolation links. The actor physics 0x0072D960 walks Actor.LatentActions (+0x1A0 data, +0x1A4 count; 0x00411644),
// casts each entry to the sequence class (static class pointer 0x00E58EB0, cast 0x0040EE90) and asks that sequence for
// the group naming the actor (0x00795E10: GroupInst +0x104 data / +0x108 count, group +0x3C actor), then dereferences
// the result without a null check (0x0041166C).
constexpr uint32_t kLatentActions = 0x1A0, kGroupInst = 0x104;
constexpr uintptr_t kSeqClass = 0x00E58EB0;
// Level visibility tree at GWorld +0x1B4: node {+0 record, +4 children data, +8 count}; record +0x40 an object whose
// outermost package names the node (0x004ADD20), record +0x5C bit 0 tested by the "level ready" query 0x004B4660
// (script native 0x008C3870). At the X-ray start that native returned 1 in the original and 0 in the replay, in the
// 03:13 probe before CinemaRestore as in its runs (2026-11-02): the bit is saved, and logged and put back at a load.
constexpr uint32_t kVisTree = 0x1B4, kVisLevel = 0x40, kVisFlags = 0x5C;
// Levels added after S (the X-ray's level, at its start). The add (0x00888440: a record made by 0x007F4E40, a node
// {record, children data, count} of 0x14 bytes appended to the root's children) runs the level's Kismet setup that
// fires the X-ray's sequence; a replay that found the abandoned timeline's node took the "already there" path and
// started the X-ray a frame late (probe 12, 2026-11-02). The game's own teardown 0x004B3860 (thiscall on its load
// request: +0x38 0 = also drop the package's objects, else keep them; +0x50 the node; arg = &package FName, read only
// when dropping) removes it: UWorld 0x006D9C80(record), GWorld +0x1C4 RemoveItem, record vt+0x60, the root's children
// Remove, node free 0x004ADFE0. Called with a stand-in request in the keeping mode, the replay re-adds the level.
constexpr uintptr_t kLevelTeardown = 0x004B3860;
using LevelTeardownFn = void(__thiscall*)(void* request, const uint32_t* packageName);

using DestroyActorFn = int(__thiscall*)(void* world, void* actor, int netForce, int modifyLevel);
const DestroyActorFn gDestroyActor = reinterpret_cast<DestroyActorFn>(kDestroyActor);

uint32_t Safe32(uintptr_t a) {
    uint32_t v = 0;
    CrashLog::SafeRead32(a, &v);
    return v;
}
uint32_t& At(uintptr_t a) { return *reinterpret_cast<uint32_t*>(a); }

// A GMalloc block start and its size, allocated (Snapshot.cpp LiveBlockInfo: medium-heap chunks carry an in-use bit).
bool BlockSize(uint32_t p, uint32_t* size) {
    unsigned info[2] = {0, 0};
    if (!p || (p & 3) || !CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(p), info)) return false;
    if (info[1] > 0x800 && info[1] <= 0x40000 && !(Safe32(p - 4) & 1)) return false;
    *size = info[1];
    return true;
}
bool LiveObject(uint32_t o, uint32_t idx) {
    const uint32_t objs = Safe32(kObjects), cnt = Safe32(kObjectCount);
    return o && !(o & 3) && objs && idx < cnt && Safe32(objs + 4 * idx) == o;
}
bool LiveActor(uint32_t a) { return a && !(a & 3) && LiveObject(a, Safe32(a + 4)) && !(Safe32(a + kActorFlags) & kDeleteMe); }
void BaseName(uint32_t obj, char* out, size_t n) {
    Ue3Names::NameOf(obj, out, n);
    if (char* u = strrchr(out, '_')) *u = 0;  // "SeqAct_Cinema_0" -> "SeqAct_Cinema"
}

struct ArrInfo {
    uint32_t off, elem;  // TArray offset in the object, element size (0: unknown)
    bool objs;           // elements are object references
};
int gInnerLogged = 0;
struct ClassInfo {
    bool actor = false;    // an Actor subclass: its birth frame is kept (interpolation links, OnLoad)
    bool tracked = false;
    bool dynamic = false;  // runtime objects (instances, SeqAct_Cinema, MKCinemaSequence): arrays saved, blocks kept
    bool spawned = false;  // CinemaGroupInstSpawned*
    bool interp = false;   // InterpGroupInst/InterpTrackInst: only those a cinematic owns (Outer chain)
    bool kismet = false;     // a Kismet object tracked for that alone (restored only with gKismet)
    bool container = false;  // a Kismet sequence container (op stack +0xD8, queued activations +0xF0)
    bool seq = false;        // any Kismet sequence object, cinematic ones included (traced by 'kismettrace')
    uint32_t size = 0;     // PropertiesSize (UStruct +0x40)
    std::vector<ArrInfo> arrays;  // ArrayProperty offsets (UProperty +0x58), TArray {data, num, max}
    std::vector<uint32_t> objFields;  // object reference fields (Object/Component/ClassProperty, each array index)
};
std::unordered_map<uint32_t, ClassInfo> gClasses;
uint32_t gActorClass = 0;

// SuperStruct chain (+0x38) contains base.
bool Derives(uint32_t cls, uint32_t base) {
    for (uint32_t c = cls, g = 0; c && g < 24; c = Safe32(c + 0x38), ++g)
        if (c == base) return true;
    return false;
}
bool IsActorClass(uint32_t cls) {
    if (gActorClass) return Derives(cls, gActorClass);
    for (uint32_t c = cls, g = 0; c && g < 24; c = Safe32(c + 0x38), ++g) {
        char cn[96];
        BaseName(c, cn, sizeof cn);
        if (!strcmp(cn, "Actor")) {
            gActorClass = c;
            return true;
        }
    }
    return false;
}

// Kismet. Sequence containers (a class named "Sequence" whose super is "SequenceOp", and subclasses) keep a pending-op
// stack (+0xD8/+0xDC, popped by 0x0073A590) and queued activations (+0xF0/+0xF4, 0x10 each), run by 0x0073AB80. A load
// across the X-ray start left an activation of the abandoned timeline there: the replay activated the X-ray's
// SeqAct_Cinema from that loop (0x0073ABB0 -> 0x0073A6E9 -> 0x00417B96, an MK object of type 0xA4 made at 0x004B2A90)
// before the frame's VM gate, the original after it. With the stack restored the replay started the X-ray two frames
// late instead (no VM events after #8 at S+2): the other sequence objects (events' trigger counts and activation times,
// ops' active flags, link impulses) are restored too. A stage Matinee (SeqAct_Interp) stays live with its instances.
// Returns 0 (not Kismet), 1 (a container) or 2 (another sequence object).
int KismetKind(uint32_t cls, const char* name) {
    bool object = false, container = false, matinee = false;
    for (uint32_t c = cls, g = 0; c && g < 24; c = Safe32(c + 0x38), ++g) {
        char cn[96];
        BaseName(c, cn, sizeof cn);
        if (!strcmp(cn, "SequenceObject")) {
            object = true;
        } else if (!strcmp(cn, "SeqAct_Interp")) {
            matinee = true;
        } else if (!strcmp(cn, "Sequence")) {
            char sn[96];
            BaseName(Safe32(c + 0x38), sn, sizeof sn);
            container |= !strcmp(sn, "SequenceOp");
        }
    }
    if (container) return 1;
    return object && (!matinee || strstr(name, "Cinema")) ? 2 : 0;
}

const ClassInfo& Classify(uint32_t cls) {
    auto it = gClasses.find(cls);
    if (it != gClasses.end()) return it->second;
    ClassInfo& ci = gClasses[cls];
    ci.actor = IsActorClass(cls);
    char name[96];
    BaseName(cls, name, sizeof name);
    const bool cinema = strstr(name, "Cinema") || !strncmp(name, "InterpGroupInst", 15) || !strncmp(name, "InterpTrackInst", 15);
    const int kk = ci.actor ? 0 : KismetKind(cls, name);
    const bool kismet = kk != 0;
    ci.kismet = kismet && !cinema;  // 'abtest kismet' switches these only
    ci.container = kk == 1;
    ci.seq = kismet;
    if (!kismet && !cinema) return ci;
    // Never an actor (raw bytes would put back component and Base pointers): none was seen, keep it that way.
    if (ci.actor) {
        Logger::Info("CINEMA class %s is an actor: not tracked", name);
        return ci;
    }
    ci.size = Safe32(cls + 0x40);
    if (ci.size <= 0x3C || ci.size > kObjectMax) {
        Logger::Info("CINEMA class %s: PropertiesSize 0x%X, not tracked", name, ci.size);
        return ci;
    }
    ci.tracked = true;
    ci.dynamic = kismet || strstr(name, "Inst") || !strncmp(name, "SeqAct_Cinema", 13) || !strncmp(name, "MKCinemaSequence", 16);
    ci.spawned = !strncmp(name, "CinemaGroupInstSpawned", 22);
    ci.interp = !kismet && !strstr(name, "Cinema");
    // UStruct Children +0x58, UField Next +0x3C, UProperty ArrayDim +0x44 / Offset +0x58 (Snapshot.cpp ReflectDataMask),
    // SuperStruct +0x38. Functions and other fields share the chain: skipped by their class name.
    for (uint32_t c = cls, depth = 0; c && depth < 16; c = Safe32(c + 0x38), ++depth) {
        uint32_t p = Safe32(c + 0x58);
        for (int guard = 0; p && guard < 2048; ++guard, p = Safe32(p + 0x3C)) {
            char pn[64];
            BaseName(Safe32(p + 0x30), pn, sizeof pn);
            if (!strcmp(pn, "ObjectProperty") || !strcmp(pn, "ComponentProperty") || !strcmp(pn, "ClassProperty")) {
                const uint32_t off = Safe32(p + 0x58), dim = Safe32(p + 0x44);
                for (uint32_t k = 0; k < dim && k < 64; ++k)
                    if (off >= 0x3C && !(off & 3) && off + 4 * k + 4 <= ci.size) ci.objFields.push_back(off + 4 * k);
                continue;
            }
            if (strcmp(pn, "ArrayProperty")) continue;
            const uint32_t off = Safe32(p + 0x58), dim = Safe32(p + 0x44);
            if (dim != 1 || off < 0x3C || (off & 3) || off + 12 > ci.size ||
                std::any_of(ci.arrays.begin(), ci.arrays.end(), [&](const ArrInfo& a) { return a.off == off; }))
                continue;
            // The element property: UArrayProperty Inner +0x78 (where UStructProperty keeps Struct), ElementSize +0x48.
            ArrInfo ai{off, 0, false};
            const uint32_t inner = Safe32(p + 0x78);
            char in[64] = "";
            if (inner) BaseName(Safe32(inner + 0x30), in, sizeof in);
            const size_t il = strlen(in);
            if (il > 8 && !strcmp(in + il - 8, "Property")) {
                ai.elem = Safe32(inner + 0x48);
                ai.objs = !strcmp(in, "ObjectProperty") || !strcmp(in, "ComponentProperty") || !strcmp(in, "ClassProperty");
                if (ai.elem > 0x400) ai.elem = 0;
            } else if (gInnerLogged++ < 5) {
                Logger::Info("CINEMA class %s array +0x%X: element property not recognised (%s)", name, off, in);
            }
            ci.arrays.push_back(ai);
        }
    }
    int objArrays = 0;
    for (const ArrInfo& a : ci.arrays) objArrays += a.objs;
    Logger::Info("CINEMA class %s %08X: size 0x%X, %u array(s) (%d of objects), %u object field(s)%s%s%s", name, cls, ci.size,
                 (unsigned)ci.arrays.size(), objArrays, (unsigned)ci.objFields.size(), ci.dynamic ? ", runtime" : "",
                 ci.spawned ? ", spawns actors" : "", kk == 1 ? ", Kismet sequence" : kk == 2 ? ", Kismet object" : "");
    return ci;
}

// The live cinematic objects, kept by diffing GObjObjects against its last copy (a few tens of microseconds; the
// first pass after a restart classifies every object once).
struct Tracked {
    uint32_t idx, size;
    const ClassInfo* ci;
};
std::vector<uint32_t> gMirror;
std::unordered_map<uint32_t, Tracked> gTracked;
// Every actor and the frame of the first save that saw it (0: there before the timeline began). An actor first seen
// at a load was born after the last save, itself at or after any load point.
struct ActorBirth {
    uint32_t idx, born;
};
std::unordered_map<uint32_t, ActorBirth> gActors;
uint32_t gLastSave = 0;

void Consider(uint32_t o, uint32_t idx, uint32_t born) {
    const ClassInfo& ci = Classify(Safe32(o + 0x30));
    if (ci.actor) {
        gActors[o] = {idx, born};
        return;
    }
    if (!ci.tracked) return;
    char name[32];
    Ue3Names::NameOf(o, name, sizeof name);
    if (!strncmp(name, "Default__", 9)) return;  // class default object
    if (ci.interp) {  // a stage's own Matinee (SeqAct_Interp, not restored) keeps its instances live, as before
        bool cine = false;
        for (uint32_t up = Safe32(o + 0x24), g = 0; up && g < 4 && !cine; up = Safe32(up + 0x24), ++g) {
            char cn[96];
            BaseName(Safe32(up + 0x30), cn, sizeof cn);
            cine = strstr(cn, "Cinema") != nullptr;
        }
        if (!cine) return;
    }
    uint32_t block = 0;
    if (!BlockSize(o, &block) || block <= 0x3C) return;
    gTracked[o] = {idx, std::min(ci.size, block), &ci};
}
void Track(uint32_t born) {
    const uint32_t objs = Safe32(kObjects), cnt = Safe32(kObjectCount);
    if (!objs || cnt > 0x200000) return;
    for (uint32_t i = cnt; i < gMirror.size(); ++i)
        if (gMirror[i]) {
            gTracked.erase(gMirror[i]);
            gActors.erase(gMirror[i]);
        }
    gMirror.resize(cnt, 0);
    const uint32_t* live = reinterpret_cast<const uint32_t*>(objs);
    for (uint32_t i = 0; i < cnt; ++i) {
        const uint32_t o = live[i] & 3 ? 0 : live[i];
        uint32_t& m = gMirror[i];
        if (o == m) continue;
        if (m) {
            auto t = gTracked.find(m);
            if (t != gTracked.end() && t->second.idx == i) gTracked.erase(t);
            auto a = gActors.find(m);
            if (a != gActors.end() && a->second.idx == i) gActors.erase(a);
        }
        m = o;
        if (o) Consider(o, i, born);
    }
}

struct ArrRec {
    uint32_t off, data, size, at;  // object offset of the TArray, its block, saved bytes, position in bytes
    const ArrInfo* info;
};
struct Rec {
    uint32_t obj, idx, size, at;  // object, GObjObjects index, bytes saved from +0x3C .. size, position in bytes
    uint32_t arrFirst, arrCount;
    uint32_t actorFlags;  // spawned groups: their actor's +0x80 at the save (the teardown clears bits 0x11)
    const ClassInfo* ci;
};
struct VisRec {
    uint32_t rec, flags;
};
struct Frame {
    uint32_t gf = 0xFFFFFFFF;
    std::vector<Rec> recs;  // sorted by obj
    std::vector<ArrRec> arrs;
    std::vector<uint8_t> bytes;
    std::vector<VisRec> vis;
    std::vector<uint32_t> kids;  // the visibility tree root's child nodes
    std::vector<uint32_t> deadAtSave;  // declared object-field values that named no live object at the save (sorted)
};
Frame gRing[kRing];
uint32_t gUndoLogged = 0, gUndone = 0;
uint32_t gVisLogged = 0, gVisPut = 0;
// levelredo: root nodes a load found added after S (the abandoned timeline's X-ray level), kept until a lookup of the
// level finds one: then it is taken out (keep mode, as levelundo) and the lookup answers "not there", so the add path
// (record, node, the world's processing of the pending record and the level's Kismet setup) runs at the same point of
// the same frame as in the pass that first added it. levelundo took them out at the load instead (probe 13: the replay
// then started the X-ray two frames early). Sticky: later saves list the leftover node as a root child too.
struct Abandoned {
    uint32_t node, rec, level, world, S;
};
std::vector<Abandoned> gAbandoned;
uint32_t gRedone = 0, gRedoLogged = 0;

void WalkVis(uint32_t node, std::vector<VisRec>& out, int depth) {
    if (!node || (node & 3) || depth > 8 || out.size() >= 256) return;
    if (const uint32_t rec = Safe32(node)) out.push_back({rec, Safe32(rec + kVisFlags)});
    const uint32_t data = Safe32(node + 4), num = Safe32(node + 8);
    for (uint32_t k = 0; data && k < num && k < 64; ++k) WalkVis(Safe32(data + 4 * k), out, depth + 1);
}

struct Deferred {
    uint32_t world, group, actor, vtable, nameIndex, gf;
};
std::vector<Deferred> gDeferred;  // game thread (the cinematic tick runs in the world tick)
bool gInstalled = false, gDeferActive = false, gFighting = false;
uint32_t gFrame = 0;
uint32_t gLoads = 0, gLogged = 0, gBigArrays = 0, gGoneLoads = 0, gLinkLogged = 0, gKismetLogged = 0;
uint32_t gDeadAtSaveRefs = 0;
uint32_t gNulledElems = 0, gKeptFields = 0, gStaleHeaders = 0, gDeadRefLoads = 0, gDeadRefLogged = 0, gStaleHdrLogged = 0;  // cinemadeadrefs
uint32_t gLostRefs = 0;  // references to spawned groups' actors destroyed since S, cleared at loads
// What the last OnLoad wrote back (object bytes from +0x3C, saved arrays): FreeQuarantine::ProtectReferenced scans them,
// since this restore runs after the quarantine's own requeue scan.
std::vector<std::pair<uintptr_t, uint32_t>> gRestoredRanges;  // object-field values put back although dead: they were dead at the save too (cinemadeadok)
uint32_t gTotRestored = 0, gTotOrphans = 0, gTotDestroyed = 0, gTotGivenBack = 0, gTotDeferred = 0;

// 'kismettrace': changes between this pass's consecutive saves, frames log.
uint32_t gTraceFrom = 0, gTraceUntil = 0, gTraceLines = 0;
constexpr uint32_t kTraceMaxLines = 6000;
void TraceDiff(const Frame& p, const Frame& f, uint32_t gf) {
    const char pass = Rollback::gIsResimulating ? 'R' : 'O';
    for (const Rec& r : f.recs) {
        if (!r.ci->seq || gTraceLines >= kTraceMaxLines) continue;
        auto it = std::lower_bound(p.recs.begin(), p.recs.end(), r.obj, [](const Rec& a, uint32_t v) { return a.obj < v; });
        char name[64];
        Ue3Names::NameOf(r.obj, name, sizeof name);
        if (it == p.recs.end() || it->obj != r.obj) {
            Logger::Frame("KTRACE gf=%u %c %s %08X: new", gf, pass, name, r.obj);
            ++gTraceLines;
            continue;
        }
        char buf[400];
        int n = 0, diffs = 0;
        for (uint32_t k = 0; k + 4 <= std::min(r.size, it->size) - 0x3C; k += 4) {
            uint32_t a, b;
            memcpy(&a, p.bytes.data() + it->at + k, 4);
            memcpy(&b, f.bytes.data() + r.at + k, 4);
            if (a == b) continue;
            if (diffs++ < 8 && n < (int)sizeof buf - 40) n += snprintf(buf + n, sizeof buf - n, " +%X:%X>%X", 0x3C + k, a, b);
        }
        for (uint32_t k = 0; k < r.arrCount; ++k) {
            const ArrRec& ar = f.arrs[r.arrFirst + k];
            for (uint32_t j = 0; j < it->arrCount; ++j) {
                const ArrRec& pa = p.arrs[it->arrFirst + j];
                if (pa.off != ar.off) continue;
                for (uint32_t q = 0; q + 4 <= std::min(pa.size, ar.size); q += 4) {
                    uint32_t a, b;
                    memcpy(&a, p.bytes.data() + pa.at + q, 4);
                    memcpy(&b, f.bytes.data() + ar.at + q, 4);
                    if (a == b) continue;
                    if (diffs++ < 8 && n < (int)sizeof buf - 40) n += snprintf(buf + n, sizeof buf - n, " [%X]+%X:%X>%X", ar.off, q, a, b);
                }
            }
        }
        if (!diffs) continue;
        char cn[64];
        Ue3Names::NameOf(Safe32(r.obj + 0x30), cn, sizeof cn);
        Logger::Frame("KTRACE gf=%u %c %s (%s) %08X: %d dword(s)%s", gf, pass, name, cn, r.obj, diffs, buf);
        ++gTraceLines;
    }
    for (const VisRec& v : f.vis) {
        auto pv = std::find_if(p.vis.begin(), p.vis.end(), [&](const VisRec& x) { return x.rec == v.rec; });
        if (pv != p.vis.end() && pv->flags == v.flags) continue;
        Logger::Frame("KTRACE gf=%u %c level record %08X +0x5C %s%08X>%08X", gf, pass, v.rec, pv == p.vis.end() ? "(joined) " : "",
                      pv == p.vis.end() ? 0 : pv->flags, v.flags);
        ++gTraceLines;
    }
    for (const VisRec& v : p.vis)
        if (std::none_of(f.vis.begin(), f.vis.end(), [&](const VisRec& x) { return x.rec == v.rec; })) {
            Logger::Frame("KTRACE gf=%u %c level record %08X left the tree", gf, pass, v.rec);
            ++gTraceLines;
        }
}

bool StillOurs(const Deferred& d) {
    return LiveActor(d.actor) && Safe32(d.actor) == d.vtable && Safe32(d.actor + 0x28) == d.nameIndex;
}
void ReallyDestroy(const Deferred& d) {
    const uint32_t world = Safe32(kGWorld);
    if (!world || world != d.world || !StillOurs(d)) {
        Logger::Frame("CINEDEFER drop %08X (world %08X, deferred in %08X)", d.actor, world, d.world);
        return;
    }
    gDestroyActor(reinterpret_cast<void*>(world), reinterpret_cast<void*>(d.actor), 0, 1);
}
// An actor spawned in the abandoned timeline: destroyed the way the teardown does it.
bool DestroyNow(uint32_t a) {
    const uint32_t world = Safe32(kGWorld);
    if (!world || !LiveActor(a)) return false;
    At(a + kActorFlags) &= ~kTeardownBits;
    gDestroyActor(reinterpret_cast<void*>(world), reinterpret_cast<void*>(a), 0, 1);
    return true;
}

}  // namespace

extern "C" int __fastcall MK9R_CineDestroyImpl(void* world, uint32_t group, void* actor, int netForce, int modifyLevel) {
    const uint32_t a = reinterpret_cast<uint32_t>(actor);
    if (!gDeferActive || !gFighting || !group || !LiveActor(a)) return gDestroyActor(world, actor, netForce, modifyLevel);
    gDeferred.push_back({reinterpret_cast<uint32_t>(world), group, a, Safe32(a), Safe32(a + 0x28), gFrame});
    ++gTotDeferred;
    Logger::Frame("CINEDEFER defer actor=%08X group=%08X gf=%u", a, group, gFrame);
    return 1;
}

// At 0x004BB852 ESI is the group (0x004BB821 mov esi, ecx); EDX is free in a thiscall.
extern "C" void MK9R_HkCineDestroy();
asm(R"(
    .text
    .globl _MK9R_HkCineDestroy
_MK9R_HkCineDestroy:
    movl %esi, %edx
    jmp @MK9R_CineDestroyImpl@20
)");

// Blend collapse guard. 0x004B4C20 (from a cinematic track, 0x004B6CE4) replaces a two-child MK blend node with its
// valid child (handle lookup 0x004CBFB0: child 0 if valid, else child 1) and deletes the node; the child is used
// unchecked (0x004B4C6D -> 0x004CCE50). Soak 14002 (2026-11-02, 3 frames after a full restore): both handles stale ->
// crash at 0x004CCE5D. With no valid child the routine now returns through its epilogue (0x004B4CE2: pop ebx, edi,
// esi, ebp; ret) and leaves the node; the hits are counted.
extern "C" {
uint32_t MK9R_gBlendGuardHits = 0;
void MK9R_HkBlendCollapse();
}
asm(R"(
    .text
    .globl _MK9R_HkBlendCollapse
_MK9R_HkBlendCollapse:
    testl %ecx, %ecx
    jz 1f
    movl $0x004CCE50, %eax
    jmp *%eax
1:
    incl _MK9R_gBlendGuardHits
    addl $8, %esp
    movl $0x004B4CE2, %eax
    jmp *%eax
)");

// The level-ready query (0x004B4660, called by script native 0x008C3870 at 0x008C388D) reads the level visibility
// tree and the world's streaming state, which no load restores: after a load across an X-ray start, the record the
// X-ray added stays (pending, +0x5C 0x21) and the replay's query took another path and answered 0 where the first
// pass had 1; the replay started the X-ray later (the one sync error left, 2026-11-02). A replayed frame now gets the
// answers the first pass of that frame got, in call order; a call the first pass did not make gets the live answer.
namespace {
constexpr uintptr_t kReadyCallSite = 0x008C388D, kLevelReady = 0x004B4660;
using LevelReadyFn = int(__thiscall*)(void* owner);
struct ReadyFrame {
    uint32_t gf = 0xFFFFFFFF, n = 0;
    int v[8];
};
ReadyFrame gReady[kRing];
uint32_t gReadyIdx = 0, gReadyLogged = 0, gReadyUsed = 0;
bool gReadyInstalled = false;
}  // namespace

extern "C" int __fastcall MK9R_LevelReadyImpl(void* owner, int /*edx*/) {
    const int live = reinterpret_cast<LevelReadyFn>(kLevelReady)(owner);
    const uint32_t idx = gReadyIdx++;
    if (gTraceUntil && gFrame >= gTraceFrom && gFrame <= gTraceUntil && gTraceLines++ < kTraceMaxLines)
        Logger::Frame("KTRACE gf=%u %c levelready call %u: %d", gFrame, Rollback::gIsResimulating ? 'R' : 'O', idx, live);
    ReadyFrame& r = gReady[gFrame % kRing];
    if (!Rollback::gIsResimulating) {
        if (r.gf != gFrame) r = ReadyFrame{gFrame, 0, {}};
        if (idx < 8) {
            r.v[idx] = live;
            r.n = std::max(r.n, idx + 1);
        }
        return live;
    }
    if (!CinemaRestore::gLevelReady || r.gf != gFrame || idx >= r.n) return live;
    if (r.v[idx] != live) {
        ++gReadyUsed;
        if (gReadyLogged++ < 20)
            Logger::Info("LEVELREADY gf=%u call %u: the replay's query answered %d, the first pass's %d (used)", gFrame, idx, live, r.v[idx]);
    }
    return r.v[idx];
}

// The level find-or-add 0x00888440 looks the level's package up in the visibility tree at 0x00888484 (cdecl
// 0x004ADD20(root, package) -> node or 0). levelredo: a node a load marked as added after its S is taken out here and
// "not there" is answered, so the add path runs again now.
namespace {
constexpr uintptr_t kLookupCallSite = 0x00888484, kVisLookup = 0x004ADD20;
using VisLookupFn = uint32_t(__cdecl*)(uint32_t root, uint32_t package);
bool gLookupInstalled = false;
}  // namespace

extern "C" uint32_t __cdecl MK9R_LevelLookupImpl(uint32_t root, uint32_t package) {
    const uint32_t node = reinterpret_cast<VisLookupFn>(kVisLookup)(root, package);
    if (!node || !CinemaRestore::gLevelRedo || gAbandoned.empty()) return node;
    auto it = std::find_if(gAbandoned.begin(), gAbandoned.end(), [&](const Abandoned& a) { return a.node == node; });
    if (it == gAbandoned.end()) return node;
    const Abandoned a = *it;
    gAbandoned.erase(it);
    const uint32_t rec = Safe32(node), world = Safe32(kGWorld);
    // Only the node the load saw (same record, level, world), a direct child of the root, with no children of its own.
    bool child = false;
    for (uint32_t k = 0; root && k < Safe32(root + 8) && k < 64 && !child; ++k) child = Safe32(Safe32(root + 4) + 4 * k) == node;
    if (rec != a.rec || Safe32(rec + kVisLevel) != a.level || world != a.world || !child || Safe32(node + 8)) {
        if (gRedoLogged++ < 30)
            Logger::Info("LEVELREDO gf=%u: node %08X looked up again but changed since the load to %u (record %08X/%08X, child %d): kept",
                         gFrame, node, a.S, rec, a.rec, child ? 1 : 0);
        return node;
    }
    alignas(4) uint8_t request[0x94] = {};
    *reinterpret_cast<uint32_t*>(request + 0x38) = 1;  // keep the package's objects
    *reinterpret_cast<uint32_t*>(request + 0x50) = node;
    const uint32_t noName[2] = {0, 0};
    reinterpret_cast<LevelTeardownFn>(kLevelTeardown)(request, noName);
    ++gRedone;
    if (gRedoLogged++ < 30) {
        char ln[64];
        Ue3Names::NameOf(a.level, ln, sizeof ln);
        Logger::Info("LEVELREDO gf=%u %c: node %08X (%s, added after the load to %u) taken out at its lookup: the level is added again",
                     gFrame, Rollback::gIsResimulating ? 'R' : 'O', node, ln, a.S);
    }
    return 0;
}

namespace CinemaRestore {

bool Install() {
    if (gInstalled) return true;
    if (!gLookupInstalled) {
        uint8_t exp[5] = {0xE8, 0, 0, 0, 0}, rep[5] = {0xE8, 0, 0, 0, 0};
        const int32_t lOld = (int32_t)(kVisLookup - (kLookupCallSite + 5));
        const int32_t lNew = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_LevelLookupImpl) - (kLookupCallSite + 5));
        memcpy(exp + 1, &lOld, 4);
        memcpy(rep + 1, &lNew, 4);
        gLookupInstalled = CodePatch::Write(kLookupCallSite, exp, rep, 5, "level find-or-add lookup 0x00888484 (levelredo)");
    }
    uint8_t expected[5] = {0xE8, 0, 0, 0, 0}, replacement[5] = {0xE8, 0, 0, 0, 0};
    const int32_t relOld = (int32_t)(kDestroyActor - (kCallSite + 5));
    const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkCineDestroy) - (kCallSite + 5));
    memcpy(expected + 1, &relOld, 4);
    memcpy(replacement + 1, &relNew, 4);
    gInstalled = CodePatch::Write(kCallSite, expected, replacement, 5, "cinema spawned-actor teardown -> DestroyActor (deferred)");
    if (!gInstalled) Logger::Info("CINEMA: call site 0x004BB852 not as expected; cinematic actor deaths stay irreversible");
    if (!gReadyInstalled) {
        const int32_t rOld = (int32_t)(kLevelReady - (kReadyCallSite + 5));
        const int32_t rNew = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_LevelReadyImpl) - (kReadyCallSite + 5));
        memcpy(expected + 1, &rOld, 4);
        memcpy(replacement + 1, &rNew, 4);
        gReadyInstalled = CodePatch::Write(kReadyCallSite, expected, replacement, 5, "level-ready query (native 0x008C3870) -> first-pass answers in replays");
    }
    static bool blendGuard = false;
    if (!blendGuard) {
        constexpr uintptr_t kBlendSite = 0x004B4C6D, kBlendUse = 0x004CCE50;
        const int32_t bOld = (int32_t)(kBlendUse - (kBlendSite + 5));
        const int32_t bNew = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkBlendCollapse) - (kBlendSite + 5));
        memcpy(expected + 1, &bOld, 4);
        memcpy(replacement + 1, &bNew, 4);
        blendGuard = CodePatch::Write(kBlendSite, expected, replacement, 5, "blend collapse 0x004B4C20: no valid child -> return");
    }
    return gInstalled;
}

void SetActive(bool on) { gDeferActive = on && gInstalled; }
void SetFrame(uint32_t gf) {
    gFrame = gf;
    gReadyIdx = 0;
}
void SetFighting(bool fought) { gFighting = fought; }

void Save(uint32_t gf, std::vector<uint32_t>* blocks) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    struct Timer {  // per-save cost, logged every 600 saves
        LARGE_INTEGER& t0;
        LARGE_INTEGER& t1;
        const Frame& f;
        ~Timer() {
            static double acc = 0, worst = 0;
            static uint32_t n = 0;
            static LARGE_INTEGER fq = {};
            if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
            QueryPerformanceCounter(&t1);
            const double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
            acc += ms;
            worst = std::max(worst, ms);
            if (++n % 600 == 0) {
                Logger::Info("CINEMA save: %.3f ms average, %.3f ms worst over 600 saves (last: %u objects, %u arrays, %u KB)", acc / 600,
                             worst, (unsigned)f.recs.size(), (unsigned)f.arrs.size(), (unsigned)(f.bytes.size() / 1024));
                acc = worst = 0;
            }
        }
    };
    Frame& f = gRing[gf % kRing];
    Timer timer{t0, t1, f};
    static uint32_t blendSeen = 0;
    if (MK9R_gBlendGuardHits != blendSeen) {
        blendSeen = MK9R_gBlendGuardHits;
        Logger::Info("BLENDGUARD gf=%u%s: a cinematic blend collapse found no valid child and left the node (%u so far)", gf,
                     Rollback::gIsResimulating ? " (replay)" : "", blendSeen);
    }
    f.gf = 0xFFFFFFFF;
    f.recs.clear();
    f.arrs.clear();
    f.bytes.clear();
    f.deadAtSave.clear();
    Track(gf);  // saved even with the restore off, so that 'abtest cinema' can switch it per load
    gLastSave = gf;
    f.recs.reserve(gTracked.size());
    for (const auto& [o, t] : gTracked) {
        if (!LiveObject(o, t.idx)) continue;
        Rec r{o, t.idx, t.size, (uint32_t)f.bytes.size(), (uint32_t)f.arrs.size(), 0, 0, t.ci};
        f.bytes.insert(f.bytes.end(), reinterpret_cast<const uint8_t*>(o + 0x3C), reinterpret_cast<const uint8_t*>(o + t.size));
        if (t.ci->dynamic) {
            blocks->push_back(o);  // a garbage collection's free of it is deferred through the window
            for (const ArrInfo& ai : t.ci->arrays) {
                const uint32_t off = ai.off, data = Safe32(o + off);
                uint32_t bs = 0;
                if (!data || !BlockSize(data, &bs) || !bs) continue;
                if (bs > kArrayMax) {
                    if (gBigArrays++ < 10) {
                        char cn[64];
                        Ue3Names::NameOf(Safe32(o + 0x30), cn, sizeof cn);
                        Logger::Info("CINEMA %s %08X +0x%X: array block of %u bytes not saved by content", cn, o, off, bs);
                    }
                    continue;
                }
                f.arrs.push_back({off, data, bs, (uint32_t)f.bytes.size(), &ai});
                f.bytes.insert(f.bytes.end(), reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data + bs));
                blocks->push_back(data);
                ++r.arrCount;
            }
        }
        if (t.ci->spawned) {
            const uint32_t a = Safe32(o + kGroupActor);
            r.actorFlags = a ? Safe32(a + kActorFlags) : 0;
        }
        for (uint32_t off : t.ci->objFields) {  // dangling already (cinemadeadok)
            if (off + 4 > t.size) continue;
            const uint32_t v = Safe32(o + off);
            if (v && !LiveObject(v, Safe32(v + 4))) f.deadAtSave.push_back(v);
        }
        f.recs.push_back(r);
    }
    std::sort(f.recs.begin(), f.recs.end(), [](const Rec& a, const Rec& b) { return a.obj < b.obj; });
    std::sort(f.deadAtSave.begin(), f.deadAtSave.end());
    f.deadAtSave.erase(std::unique(f.deadAtSave.begin(), f.deadAtSave.end()), f.deadAtSave.end());
    f.vis.clear();
    f.kids.clear();
    if (const uint32_t world = Safe32(kGWorld)) {
        const uint32_t root = Safe32(world + kVisTree);
        WalkVis(root, f.vis, 0);
        const uint32_t data = root ? Safe32(root + 4) : 0, num = root ? Safe32(root + 8) : 0;
        for (uint32_t k = 0; data && k < num && k < 64; ++k) f.kids.push_back(Safe32(data + 4 * k));
    }
    f.gf = gf;
    if (gTraceUntil && gf > gTraceFrom && gf <= gTraceUntil) {
        const Frame& p = gRing[(gf - 1) % kRing];
        if (p.gf == gf - 1) TraceDiff(p, f, gf);
    }
}

const std::vector<std::pair<uintptr_t, uint32_t>>& LastRestoredRanges() { return gRestoredRanges; }
std::vector<std::pair<uintptr_t, uint32_t>> gGoneRanges;
const std::vector<std::pair<uintptr_t, uint32_t>>& LastGoneRanges() { return gGoneRanges; }

int OnLoad(uint32_t S) {
    ++gLoads;
    gRestoredRanges.clear();
    gGoneRanges.clear();
    const Frame& f = gRing[S % kRing];
    const bool have = gRestore && f.gf == S;
    if (f.gf == S && gLevelUndo) {  // levels added to the world after S: taken out again, kept loaded
        const uint32_t world = Safe32(kGWorld), root = world ? Safe32(world + kVisTree) : 0;
        for (int k = root ? (int)Safe32(root + 8) - 1 : -1; k >= 0 && k < 64; --k) {
            const uint32_t node = Safe32(Safe32(root + 4) + 4 * k);
            if (!node || std::find(f.kids.begin(), f.kids.end(), node) != f.kids.end()) continue;
            const uint32_t rec = Safe32(node), sub = Safe32(node + 8);
            char ln[64];
            Ue3Names::NameOf(Safe32(rec + kVisLevel), ln, sizeof ln);
            if (!rec || sub) {
                if (gUndoLogged++ < 20)
                    Logger::Info("LEVELUNDO S=%u: node %08X (record %08X, %s) added after the save has %u child node(s): left", S, node, rec, ln, sub);
                continue;
            }
            alignas(4) uint8_t request[0x94] = {};
            *reinterpret_cast<uint32_t*>(request + 0x38) = 1;  // keep the package's objects
            *reinterpret_cast<uint32_t*>(request + 0x50) = node;
            const uint32_t noName[2] = {0, 0};
            reinterpret_cast<LevelTeardownFn>(kLevelTeardown)(request, noName);
            ++gUndone;
            if (gUndoLogged++ < 20)
                Logger::Info("LEVELUNDO S=%u: node %08X (record %08X, %s, +0x5C %08X) added after the save: taken out of the world, kept loaded",
                             S, node, rec, ln, Safe32(rec + kVisFlags));
        }
    }
    if (f.gf == S && gLevelRedo && !gLevelUndo) {  // levels added after S: marked, taken out when looked up again
        const uint32_t world = Safe32(kGWorld), root = world ? Safe32(world + kVisTree) : 0;
        gAbandoned.erase(std::remove_if(gAbandoned.begin(), gAbandoned.end(), [&](const Abandoned& a) { return a.world != world; }),
                         gAbandoned.end());
        for (uint32_t k = 0; root && k < Safe32(root + 8) && k < 64; ++k) {
            const uint32_t node = Safe32(Safe32(root + 4) + 4 * k);
            if (!node || std::find(f.kids.begin(), f.kids.end(), node) != f.kids.end()) continue;
            if (std::any_of(gAbandoned.begin(), gAbandoned.end(), [&](const Abandoned& a) { return a.node == node; })) continue;
            const uint32_t rec = Safe32(node);
            gAbandoned.push_back({node, rec, Safe32(rec + kVisLevel), world, S});
            if (gRedoLogged++ < 30) {
                char ln[64];
                Ue3Names::NameOf(Safe32(rec + kVisLevel), ln, sizeof ln);
                Logger::Info("LEVELREDO S=%u: node %08X (record %08X, %s, +0x5C %08X) was added after the save: re-added when next "
                             "looked up", S, node, rec, ln, Safe32(rec + kVisFlags));
            }
        }
    }
    if (f.gf == S) {  // level visibility records: only those still in the live tree are touched
        std::vector<VisRec> live;
        if (const uint32_t world = Safe32(kGWorld)) WalkVis(Safe32(world + kVisTree), live, 0);
        for (const VisRec& v : f.vis) {
            auto it = std::find_if(live.begin(), live.end(), [&](const VisRec& l) { return l.rec == v.rec; });
            const bool there = it != live.end();
            if (there && it->flags == v.flags) continue;
            const bool put = gLevelVis && there && ((it->flags ^ v.flags) & 1);
            if (gVisLogged++ < 40) {
                char ln[64];
                Ue3Names::NameOf(Safe32(v.rec + kVisLevel), ln, sizeof ln);
                Logger::Info("LEVELVIS S=%u: record %08X (%s) +0x5C saved %08X, %s%08X%s", S, v.rec, ln, v.flags,
                             there ? "live " : "no longer in the tree, ", there ? it->flags : 0, put ? ": bit 0 put back" : "");
            }
            if (put) {
                At(v.rec + kVisFlags) = (it->flags & ~1u) | (v.flags & 1);
                ++gVisPut;
            }
        }
        for (const VisRec& l : live)
            if (std::none_of(f.vis.begin(), f.vis.end(), [&](const VisRec& v) { return v.rec == l.rec; }) && gVisLogged++ < 40)
                Logger::Info("LEVELVIS S=%u: record %08X (+0x5C %08X) joined the tree after the save", S, l.rec, l.flags);
    }
    // Deferred teardown destroys of the abandoned timeline: decided after the restore (the group may own the actor again).
    std::vector<Deferred> undone;
    for (size_t i = 0; i < gDeferred.size();) {
        if (gDeferred[i].gf >= S) {
            undone.push_back(gDeferred[i]);
            gDeferred.erase(gDeferred.begin() + i);
        } else {
            ++i;
        }
    }
    struct KeepLive {
        uint32_t obj, off, dead, live;  // cinemadeadrefs: a field whose saved value names a dead object keeps 'live'
    };
    std::vector<KeepLive> keepLive;
    int staleHdrs = 0;
    std::vector<uint32_t> goneObjs;  // saved objects garbage-collected since the save (sorted, as the records)
    auto isGone = [&](uint32_t o) { return std::binary_search(goneObjs.begin(), goneObjs.end(), o); };
    std::vector<uint32_t> family;  // left live with an object naming collected ones (see below), sorted
    auto held = [&](uint32_t o) {
        for (uint32_t up = o, g = 0; up && g < 6 && !family.empty(); up = Safe32(up + 0x24), ++g)
            if (std::binary_search(family.begin(), family.end(), up)) return true;
        return false;
    };
    auto saved = [&](uint32_t o) -> const Rec* {
        auto it = std::lower_bound(f.recs.begin(), f.recs.end(), o, [](const Rec& r, uint32_t v) { return r.obj < v; });
        return it != f.recs.end() && it->obj == o && !isGone(o) ? &*it : nullptr;
    };
    int restored = -1, gone = 0, orphans = 0, orphanActors = 0, futureActors = 0, lost = 0, arrays = 0, badArrays = 0;
    int givenBack = 0, destroyed = 0, linkDestroyed = 0, linkCut = 0, linkMissing = 0;
    std::vector<uint32_t> lostActors;  // spawned groups' actors destroyed since S by another path (cinemadeadrefs, below)
    if (have) {
        Track(gLastSave + 1);  // births since the last save
        for (const Rec& r : f.recs)
            if (!LiveObject(r.obj, r.idx)) {
                goneObjs.push_back(r.obj);
                gGoneRanges.push_back({r.obj, r.size});
            }
        gone = (int)goneObjs.size();
        // A collected object is left out; the rest is restored only if none of its saved bytes names a collected one
        // (the collector frees what nothing references, so this holds unless the object was dropped after S).
        // References the restore would put back to objects no longer allocated. An object field saved equal to its
        // live value is no new reference (the periodic collections, every ~200 frames, collected 18 objects named
        // only by such native fields and blocked every load across them). An array counts by its saved length,
        // whatever the slack holds: a sequence's GroupInst emptied at the X-ray end keeps its old pointers, and a
        // restore that put the length back made a freed group live again (crash at a vcall of it, 2026-11-02).
        // Object arrays also check every element is still a live UObject (collected objects outside the set).
        int goneRefs = 0;
        std::vector<uint32_t> blocked;  // objects whose saved state names such objects
        auto note = [&](uint32_t owner, uint32_t off, uint32_t v, const char* what) {
            if (std::find(blocked.begin(), blocked.end(), owner) == blocked.end()) blocked.push_back(owner);
            if (++goneRefs <= 3 && gGoneLoads < 20) {
                char cn[64];
                Ue3Names::NameOf(Safe32(owner + 0x30), cn, sizeof cn);
                Logger::Info("CINEMA load S=%u: %s %08X +0x%X held %08X at the save, %s", S, cn, owner, off, v, what);
            }
        };
        for (const Rec& r : f.recs) {
            if (isGone(r.obj)) continue;
            for (uint32_t k = 0; gone && k + 4 <= r.size - 0x3C; k += 4) {
                uint32_t v;
                memcpy(&v, f.bytes.data() + r.at + k, 4);
                if (isGone(v) && Safe32(r.obj + 0x3C + k) != v) note(r.obj, 0x3C + k, v, "now collected");
            }
            // Declared object references the restore changes must name live objects (actors and other untracked
            // objects included; a Kismet variable naming an actor collected since S, say).
            for (uint32_t off : r.ci->objFields) {
                if (off + 4 > r.size) continue;
                uint32_t v;
                memcpy(&v, f.bytes.data() + r.at + (off - 0x3C), 4);
                const uint32_t lv = Safe32(r.obj + off);
                if (v && v != lv && !isGone(v) && !LiveObject(v, Safe32(v + 4))) {
                    if (gDeadRefs) {  // the live value stays (below)
                        keepLive.push_back({r.obj, off, v, lv});
                        continue;
                    }
                    if (gDeadOk && std::binary_search(f.deadAtSave.begin(), f.deadAtSave.end(), v)) {
                        ++gDeadAtSaveRefs;  // dangling at the save already: S's own state, put back
                        continue;
                    }
                    note(r.obj, off, v, "no longer a live object");
                }
            }
            for (uint32_t k = 0; k < r.arrCount; ++k) {
                const ArrRec& ar = f.arrs[r.arrFirst + k];
                uint32_t num = 0;
                memcpy(&num, f.bytes.data() + r.at + (ar.off + 4 - 0x3C), 4);
                const uint32_t len = ar.info->elem ? std::min(ar.size, num * ar.info->elem) : ar.size;
                for (uint32_t j = 0; j + 4 <= len; j += 4) {
                    uint32_t v;
                    memcpy(&v, f.bytes.data() + ar.at + j, 4);
                    if (gone && isGone(v)) {
                        note(r.obj, ar.off, v, "now collected");
                    } else if (ar.info->objs && ar.info->elem == 4 && v && !LiveObject(v, Safe32(v + 4)) && !gDeadRefs) {
                        note(r.obj, ar.off, v, "no longer a live object");  // (cinemadeadrefs: set to 0 after the restore)
                    }
                }
            }
        }
        // Such an object and its cinematic (everything whose Outer chain reaches it, and the groups its GroupInst names
        // live or saved) stay live; the rest is restored. Soak 14001: every blocked load (43, about one per 200 frames
        // in every fight) was a stage's looping SeqAct_Cinema whose saved GroupInst named the previous loop's groups,
        // and it stopped the whole restore, the X-ray's objects included.
        // Off by default (gPartial): its first use (probe 14, S=9126, one object left live) was followed 48 frames later by
        // a GC worker (0x005F95CA, vt+0x94) calling into a UObject freed at 9126; restored state presumably held a
        // reference the checks above do not see (inside struct arrays, to an untracked object). Without it a load
        // across such a collection restores nothing, as before.
        const uint32_t seqClass0 = Safe32(kSeqClass);
        auto seqClassOf = [&](uint32_t o) { return seqClass0 && Derives(Safe32(o + 0x30), seqClass0); };
        if (gPartial) family = blocked;
        for (uint32_t b : blocked) {
            if (!gPartial || !seqClassOf(b)) continue;
            const uint32_t data = Safe32(b + kGroupInst), num = Safe32(b + kGroupInst + 4);
            for (uint32_t k = 0; data && k < num && k < 256; ++k) family.push_back(Safe32(data + 4 * k));
            if (const Rec* r = saved(b)) {
                uint32_t sd = 0, sn = 0;
                memcpy(&sd, f.bytes.data() + r->at + (kGroupInst - 0x3C), 4);
                memcpy(&sn, f.bytes.data() + r->at + (kGroupInst + 4 - 0x3C), 4);
                for (uint32_t k = 0; k < r->arrCount; ++k) {
                    const ArrRec& ar = f.arrs[r->arrFirst + k];
                    if (ar.off != kGroupInst || ar.data != sd) continue;
                    for (uint32_t j = 0; j < sn && 4 * j + 4 <= ar.size; ++j) {
                        uint32_t g;
                        memcpy(&g, f.bytes.data() + ar.at + 4 * j, 4);
                        family.push_back(g);
                    }
                }
            }
        }
        std::sort(family.begin(), family.end());
        int heldCount = 0;
        for (const Rec& r : f.recs) heldCount += !isGone(r.obj) && held(r.obj);
        if (goneRefs && gGoneLoads++ < 20)
            Logger::Info("CINEMA load S=%u: %d saved object(s) name objects no longer live (%d collected since the save): %s", S,
                         (int)blocked.size(), gone, gPartial ? "those of their cinematics left live, the others restored" : "nothing restored");
        else if (gone && gGoneLoads++ < 20)
            Logger::Info("CINEMA load S=%u: %d of %u saved cinematic object(s) were garbage-collected since the save; the others are restored",
                         S, gone, (unsigned)f.recs.size());
        if (gPartial && goneRefs && gGoneLoads < 20) Logger::Info("CINEMA load S=%u: %d object(s) left live", S, heldCount);
        if (!goneRefs || gPartial) {
            // Actors the abandoned timeline spawned: by groups born after S (orphans), or by groups of S that owned
            // nothing or another actor then.
            std::vector<uint32_t> orphanObjs;
            for (const auto& [o, t] : gTracked) {
                if (held(o)) continue;  // a cinematic left live keeps its newer instances and actors
                const Rec* r = saved(o);
                if (!r) orphanObjs.push_back(o);
                if (!t.ci->spawned) continue;
                const uint32_t a = Safe32(o + kGroupActor);
                if (!(Safe32(o + kGroupFlags) & 1) || !a) continue;
                if (r) {
                    const uint8_t* b = f.bytes.data() + r->at;
                    uint32_t svActor = 0, svFlags = 0;
                    memcpy(&svActor, b + (kGroupActor - 0x3C), 4);
                    memcpy(&svFlags, b + (kGroupFlags - 0x3C), 4);
                    if ((svFlags & 1) && svActor == a) continue;  // owned at S as well
                }
                if (DestroyNow(a)) ++(r ? futureActors : orphanActors);
                if (!r) {  // unreferenced from now on; nothing may destroy that actor again
                    At(o + kGroupFlags) &= ~1u;
                    At(o + kGroupActor) = 0;
                }
            }
            // Born after S, nothing restored names them: no longer saved (their collection must not stop later loads;
            // they keep their GObjObjects slot in the mirror, so Track does not take them up again).
            orphans = (int)orphanObjs.size();
            for (uint32_t o : orphanObjs) gTracked.erase(o);
            std::sort(orphanObjs.begin(), orphanObjs.end());
            restored = 0;
            for (const Rec& r : f.recs) {
                if (isGone(r.obj) || held(r.obj)) continue;
                if (r.ci->kismet && !CinemaRestore::gKismet) continue;
                ++restored;
                if (r.ci->container) {
                    // Diagnostic: the pending-op stack and queued activations, saved vs live (counts at +0xDC / +0xF4).
                    uint32_t sStack = 0, sQueued = 0;
                    if (r.size >= 0xF8) {
                        memcpy(&sStack, f.bytes.data() + r.at + (0xDC - 0x3C), 4);
                        memcpy(&sQueued, f.bytes.data() + r.at + (0xF4 - 0x3C), 4);
                    }
                    const uint32_t lStack = Safe32(r.obj + 0xDC), lQueued = Safe32(r.obj + 0xF4);
                    if ((sStack != lStack || sQueued != lQueued) && gKismetLogged++ < 40) {
                        char sn[64];
                        Ue3Names::NameOf(r.obj, sn, sizeof sn);
                        Logger::Info("KISMET S=%u %s %08X: op stack %u at the save, %u live; queued activations %u at the save, %u live",
                                     S, sn, r.obj, sStack, lStack, sQueued, lQueued);
                    }
                }
                constexpr size_t kHdrMax = 16;
                uint32_t liveHdr[kHdrMax][3];
                const size_t nHdr = std::min(r.ci->arrays.size(), kHdrMax);
                for (size_t a = 0; a < nHdr; ++a)
                    for (int w = 0; w < 3; ++w) liveHdr[a][w] = Safe32(r.obj + r.ci->arrays[a].off + 4 * w);
                memcpy(reinterpret_cast<void*>(r.obj + 0x3C), f.bytes.data() + r.at, r.size - 0x3C);
                gRestoredRanges.push_back({r.obj + 0x3C, r.size - 0x3C});
                // cinemadeadrefs: a restored TArray header naming a block the game had freed before S (dangling at the
                // save: the save copied a header an earlier load had left behind) keeps the live header; its saved
                // contents are then skipped below (the header no longer names the saved block).
                for (size_t a = 0; gDeadRefs && a < nHdr; ++a) {
                    const uint32_t off = r.ci->arrays[a].off, rd = Safe32(r.obj + off);
                    uint32_t freedGf = 0;
                    if (off + 12 > r.size || !rd || rd == liveHdr[a][0] || !FreeQuarantine::FreedBefore(reinterpret_cast<void*>(rd), S, &freedGf))
                        continue;
                    for (int w = 0; w < 3; ++w) At(r.obj + off + 4 * w) = liveHdr[a][w];
                    ++staleHdrs;
                    if (gStaleHdrLogged < 40) {
                        ++gStaleHdrLogged;
                        char cn[64];
                        Ue3Names::NameOf(r.obj, cn, sizeof cn);
                        Logger::Info("CINEMA load S=%u: %s %08X +0x%X: saved array block %08X was freed at gf %u (held); live header kept (%08X, %u)",
                                     S, cn, r.obj, off, rd, freedGf, liveHdr[a][0], liveHdr[a][1]);
                    }
                }
                for (uint32_t k = 0; k < r.arrCount; ++k) {
                    const ArrRec& ar = f.arrs[r.arrFirst + k];
                    if (Safe32(r.obj + ar.off) != ar.data) continue;
                    uint32_t bs = 0;
                    if (BlockSize(ar.data, &bs) && bs >= ar.size) {
                        memcpy(reinterpret_cast<void*>(ar.data), f.bytes.data() + ar.at, ar.size);
                        gRestoredRanges.push_back({ar.data, ar.size});
                        ++arrays;
                    } else {  // freed by a path the quarantine does not hold: an empty array rather than a dangling one
                        At(r.obj + ar.off) = At(r.obj + ar.off + 4) = At(r.obj + ar.off + 8) = 0;
                        ++badArrays;
                    }
                }
                if (r.ci->spawned && (Safe32(r.obj + kGroupFlags) & 1)) {
                    const uint32_t a = Safe32(r.obj + kGroupActor);
                    if (!a) continue;
                    if (!LiveActor(a)) {  // destroyed since S by another path: the group must not name it
                        At(r.obj + kGroupFlags) &= ~1u;
                        At(r.obj + kGroupActor) = 0;
                        lostActors.push_back(a);
                        ++lost;
                        continue;
                    }
                    At(a + kActorFlags) = (Safe32(a + kActorFlags) & ~kTeardownBits) | (r.actorFlags & kTeardownBits);
                }
            }
            // cinemadeadrefs: fields whose saved value names a dead object get the live value back (0 when that is not a
            // live object either), unless the restore above changed them already (a spawned group's lost actor: 0).
            for (const KeepLive& k : keepLive) {
                if (isGone(k.obj) || held(k.obj) || Safe32(k.obj + k.off) != k.dead) continue;
                const uint32_t put = k.live && LiveObject(k.live, Safe32(k.live + 4)) ? k.live : 0;
                At(k.obj + k.off) = put;
                ++gKeptFields;
                if (gDeadRefLogged < 40) {
                    ++gDeadRefLogged;
                    char cn[64];
                    Ue3Names::NameOf(k.obj, cn, sizeof cn);
                    Logger::Info("CINEMA load S=%u: %s %08X +0x%X held %08X at the save, no longer a live object: %08X kept", S, cn, k.obj,
                                 k.off, k.dead, put);
                }
            }
            // Interpolation links: an actor's LatentActions may name a tracked sequence only if the sequence's restored
            // GroupInst names the actor. A point light the abandoned timeline spawned (no group of S owned it) was still
            // driven by the X-ray sequence on the first replayed frame and crashed at 0x0041166C (2026-11-02). Such an
            // actor born after S is destroyed; an older one (the fighters' list is kept live) loses the entry.
            const uint32_t seqClass = Safe32(kSeqClass);
            auto drives = [&](uint32_t seq, uint32_t a) {
                if (held(seq)) return true;     // left live: its links are the live ones
                if (!saved(seq)) return false;  // born after S: no link to it existed at S
                const uint32_t data = Safe32(seq + kGroupInst), num = Safe32(seq + kGroupInst + 4);
                for (uint32_t k = 0; data && k < num && k < 256; ++k)
                    if (Safe32(Safe32(data + 4 * k) + kGroupActor) == a) return true;
                return false;
            };
            for (const auto& [a, ab] : gActors) {
                if (!seqClass || !LiveObject(a, ab.idx) || (Safe32(a + kActorFlags) & kDeleteMe)) continue;
                const uint32_t data = Safe32(a + kLatentActions);
                uint32_t num = Safe32(a + kLatentActions + 4);
                if (!data || !num || num > 256) continue;
                for (uint32_t k = 0; k < num;) {
                    const uint32_t seq = Safe32(data + 4 * k);
                    const bool cine = gTracked.count(seq) || std::binary_search(orphanObjs.begin(), orphanObjs.end(), seq);
                    if (!cine || !Derives(Safe32(seq + 0x30), seqClass) || drives(seq, a)) {
                        ++k;
                        continue;
                    }
                    const bool future = ab.born > S;
                    if (gLinkLogged++ < 30) {
                        char an[64], sn[64];
                        Ue3Names::NameOf(Safe32(a + 0x30), an, sizeof an);
                        Ue3Names::NameOf(seq, sn, sizeof sn);
                        Logger::Info("CINEMA load S=%u: %s %08X (first saved at gf %u) is linked to %s %08X%s: %s", S, an, a, ab.born, sn, seq,
                                     saved(seq) ? ", whose restored groups do not name it" : ", born after S",
                                     future ? "destroyed" : "link removed");
                    }
                    if (future) {
                        linkDestroyed += DestroyNow(a);
                        break;
                    }
                    for (uint32_t j = k; j + 1 < num; ++j) At(data + 4 * j) = Safe32(data + 4 * (j + 1));
                    At(a + kLatentActions + 4) = --num;
                    ++linkCut;
                }
            }
            // The other direction, diagnostic only: a restored group naming a live actor whose list lacks the sequence
            // (the actor is then not moved by it in the replay).
            for (const Rec& r : f.recs) {
                if (!seqClass || !Derives(Safe32(r.obj + 0x30), seqClass)) continue;
                const uint32_t data = Safe32(r.obj + kGroupInst), num = Safe32(r.obj + kGroupInst + 4);
                for (uint32_t k = 0; data && k < num && k < 256; ++k) {
                    const uint32_t a = Safe32(Safe32(data + 4 * k) + kGroupActor);
                    if (!LiveActor(a)) continue;
                    const uint32_t ld = Safe32(a + kLatentActions), ln = Safe32(a + kLatentActions + 4);
                    bool has = false;
                    for (uint32_t j = 0; ld && j < ln && j < 256 && !has; ++j) has = Safe32(ld + 4 * j) == r.obj;
                    linkMissing += !has;
                }
            }
        }
    }
    for (const Deferred& d : undone) {
        const Rec* r = restored >= 0 && !held(d.group) ? saved(d.group) : nullptr;
        if (r && (Safe32(d.group + kGroupFlags) & 1) && Safe32(d.group + kGroupActor) == d.actor && StillOurs(d)) {
            ++givenBack;  // its group owns it again (flags put back above)
        } else {
            ReallyDestroy(d);
            ++destroyed;
        }
    }
    // cinemadeadrefs: no object array of a tracked object may name a dead UObject after the load, restored or kept live
    // (the collector clears references to what it destroys; a load must not bring one back). 0x00414694 skips 0.
    // A spawned group's actor destroyed since S (lost, above) is still a UObject until the collector purges it, so the
    // dead-object test passes it; the restored Kismet state (object variables, Targets) still named it, nothing in the
    // replay tears it down again, and after the purge a latent action's target loop called into it (0x004146A8: batch 28
    // slowB B, net13, batch 20 x2, batch 22; every one after a "lost" load). References to those actors are cleared too.
    int nulled = 0, lostRefs = 0;
    std::sort(lostActors.begin(), lostActors.end());
    auto lostActor = [&](uint32_t v) { return !lostActors.empty() && std::binary_search(lostActors.begin(), lostActors.end(), v); };
    if (gDeadRefs) {
        for (const auto& [o, t] : gTracked) {
            if (!LiveObject(o, t.idx)) continue;
            for (uint32_t off : t.ci->objFields) {
                if (lostActors.empty() || off + 4 > t.size || !lostActor(Safe32(o + off))) continue;
                At(o + off) = 0;
                ++lostRefs;
            }
            for (const ArrInfo& ai : t.ci->arrays) {
                if (!ai.objs || ai.elem != 4 || ai.off + 12 > t.size) continue;
                const uint32_t data = Safe32(o + ai.off), num = Safe32(o + ai.off + 4);
                uint32_t bs = 0;
                if (!data || !num || num > 4096 || !BlockSize(data, &bs) || bs < num * 4 || FreeQuarantine::IsPending(reinterpret_cast<void*>(data)))
                    continue;
                for (uint32_t j = 0; j < num; ++j) {
                    uint32_t v = 0;
                    if (!CrashLog::SafeRead32(data + 4 * j, &v) || !v) continue;
                    if (LiveObject(v, Safe32(v + 4))) {
                        if (lostActor(v)) {
                            At(data + 4 * j) = 0;
                            ++lostRefs;
                        }
                        continue;
                    }
                    At(data + 4 * j) = 0;
                    ++nulled;
                    if (gDeadRefLogged < 40) {
                        ++gDeadRefLogged;
                        char cn[64];
                        Ue3Names::NameOf(o, cn, sizeof cn);
                        Logger::Info("CINEMA load S=%u: %s %08X +0x%X[%u] named %08X, no longer a live object: set to 0", S, cn, o, ai.off, j, v);
                    }
                }
            }
        }
        gNulledElems += nulled;
        gLostRefs += lostRefs;
        const int kept = (int)keepLive.size();
        if ((nulled || kept || staleHdrs || lostRefs) && (gDeadRefLoads++ < 40 || gDeadRefLoads % 200 == 0))
            Logger::Info("CINEMA load S=%u: dead references: %d array element(s) set to 0, %d field(s) found (live value kept), %d array "
                         "header(s) kept live (block freed before S), %d reference(s) to %d actor(s) destroyed since S cleared; %s "
                         "(%u loads so far)",
                         S, nulled, kept, staleHdrs, lostRefs, (int)lostActors.size(), restored >= 0 ? "restored" : "nothing restored",
                         gDeadRefLoads);
    }
    gStaleHeaders += staleHdrs;
    if (restored > 0) gTotRestored += restored;
    gTotOrphans += orphans;
    gTotDestroyed += orphanActors + futureActors + destroyed + linkDestroyed;
    gTotGivenBack += givenBack;
    const bool notable = orphans || orphanActors || futureActors || lost || badArrays || givenBack || destroyed || linkDestroyed || linkCut ||
                         linkMissing;
    if (notable && (gLogged++ < 40 || gLogged % 500 == 0))
        Logger::Info("CINEMA load S=%u: %d object(s) restored (%d array(s)%s), %d born after S left to the collector; actors destroyed: "
                     "%d of those, %d spawned after S by older groups, %d deferred teardown(s), %d linked to a sequence that no longer "
                     "drives them; %d given back, %d lost; links removed %d, missing %d",
                     S, restored, arrays, badArrays ? ", some emptied" : "", orphans, orphanActors, futureActors, destroyed, linkDestroyed,
                     givenBack, lost, linkCut, linkMissing);
    if (badArrays) Logger::Info("CINEMA load S=%u: %d array block(s) no longer allocated: emptied", S, badArrays);
    if (gLoads % 300 == 0)
        Logger::Info("CINEMA %u loads: %u objects restored, %u born after their load point, %u actors destroyed, %u given back, "
                     "%u teardowns deferred (%u pending); %u loads found saved objects collected; %u level visibility bits put back, "
                     "%u level-ready answers replaced, %u levels taken out of the world; %u dangling-at-save references put back; "
                     "dead references: %u array elements set to 0, %u fields kept live, %u array headers kept live, %u references "
                     "to actors destroyed since the load point cleared",
                     gLoads, gTotRestored, gTotOrphans, gTotDestroyed, gTotGivenBack, gTotDeferred, (unsigned)gDeferred.size(),
                     gGoneLoads, gVisPut, gReadyUsed, gUndone, gDeadAtSaveRefs, gNulledElems, gKeptFields, gStaleHeaders, gLostRefs);
    return restored;
}

void Release(uint32_t oldestNeededGf) {
    std::vector<Deferred> due;
    for (size_t i = 0; i < gDeferred.size();) {
        if (gDeferred[i].gf < oldestNeededGf) {
            due.push_back(gDeferred[i]);
            gDeferred.erase(gDeferred.begin() + i);
        } else {
            ++i;
        }
    }
    if (!due.empty()) Logger::Frame("CINEDEFER release %u (oldest needed gf %u)", (unsigned)due.size(), oldestNeededGf);
    for (const Deferred& d : due) ReallyDestroy(d);
}

void ReleaseAll() { Release(0xFFFFFFFF); }

void DropAll() {
    if (!gDeferred.empty()) Logger::Frame("CINEDEFER drop %u deferred actors at timeline restart", (unsigned)gDeferred.size());
    gDeferred.clear();
    for (Frame& f : gRing) f.gf = 0xFFFFFFFF;
    for (auto& [a, ab] : gActors) ab.born = 0;  // frame numbers may start over: every actor now predates any load point
    for (ReadyFrame& r : gReady) r.gf = 0xFFFFFFFF;
    gAbandoned.clear();
}

uint32_t Pending() { return (uint32_t)gDeferred.size(); }

void Prewarm() {
    static uint32_t calls = 0;
    if (calls++ % 30 == 0) Track(0);  // objects seen here predate any load point
}

void Trace(int frames) {
    gTraceFrom = gFrame;
    gTraceUntil = frames > 0 ? gFrame + (uint32_t)frames : 0;
    gTraceLines = 0;
}

}  // namespace CinemaRestore
