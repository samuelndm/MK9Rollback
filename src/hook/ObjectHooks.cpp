#include "ObjectHooks.h"

#include "../rollback/ActorDefer.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../debug/Lab.h"
#include "../debug/Logger.h"
#include "CodePatch.h"

namespace Rollback {
extern bool gIsResimulating;
}

namespace {
constexpr uintptr_t kCtor = 0x0042E000;
constexpr uintptr_t kDtor = 0x0042D630;

const uint8_t kCtorOrig[8] = {0x8B, 0xC1, 0x8B, 0x0D, 0x0C, 0x82, 0xDE, 0x00};  // mov eax,ecx; mov ecx,[0xDE820C]
const uint8_t kDtorOrig[6] = {0xC7, 0x01, 0x08, 0x6B, 0xC6, 0x00};              // mov dword [ecx],0xC66B08

struct Event {
    uint32_t obj, id, vtable;
    uint32_t actor;  // obj+8 at destruction (MKObj: its UE3 actor; base dtor runs after the MKObj dtor)
    bool created;
    uint8_t phase;  // ObjectHooks::gPhase at the time of the event
    uint32_t caller;
    uint32_t stack[6];  // return addresses above the ctor/dtor (who spawned or killed it)
};

// Fixed-capacity ring so the hooks never allocate (they can run on any thread).
constexpr int kRing = 8192;
Event gRing[kRing];
volatile LONG gHead = 0;  // next write
LONG gTail = 0;           // next read (game thread)
volatile LONG gDropped = 0;
volatile LONG gInFlight = 0;
volatile LONG gCreatedSinceMark = 0, gDestroyedSinceMark = 0;

std::unordered_map<uint32_t, std::pair<const char*, uint32_t>> gLive;  // obj -> (type, birth gf)

// Frame-stamped birth/death history (game-thread only). A load to snapshot frame S
// needs: objects born at gf >= S still alive (destroy them), and objects that existed
// at S and died at gf >= S (restored by the heap image; actor-owning ones are retired).
struct HistEvent {
    uint32_t gf;
    uint32_t obj, id;
    const char* type;
    bool born;
    uint32_t actor;  // deaths: obj+8 at destruction
};
std::vector<HistEvent> gHistory;
bool gTracking = false;
uint32_t gTrackingSinceGf = 0;  // history is complete from this frame on
uint32_t gMarkGf = 0;           // single-slot saveab/loadab compatibility

bool IsMKObj(const char* chain) { return !strncmp(chain, "MKObj<", 6) || strstr(chain, "<MKObj<") != nullptr; }

}  // namespace

extern "C" {
void* gMK9R_CtorTramp = nullptr;

// (gPhase/gGameThread are declared in ObjectHooks.h)
// Stubs pass the caller's return address (the instruction after the CALL into the
// ctor/dtor) and the thread id is compared with the game thread.
void __cdecl MK9R_OnCtor(uint32_t obj, uint32_t caller) {
    InterlockedIncrement(&gInFlight);
    const LONG slot = InterlockedIncrement(&gHead) - 1;
    const uint8_t phase = GetCurrentThreadId() == ObjectHooks::gGameThread ? ObjectHooks::gPhase : (uint8_t)(10 + ObjectHooks::gPhase);
    if (slot - gTail >= kRing) InterlockedIncrement(&gDropped);
    else {
        Event& e = gRing[slot % kRing];
        e = {obj, 0, 0, 0, true, phase, caller, {}};
        if (Lab::On()) RtlCaptureStackBackTrace(1, 6, reinterpret_cast<PVOID*>(e.stack), nullptr);  // OBJ lines only
    }
    InterlockedIncrement(&gCreatedSinceMark);
    InterlockedDecrement(&gInFlight);
}

void __cdecl MK9R_OnDtor(uint32_t obj, uint32_t caller) {
    InterlockedIncrement(&gInFlight);
    const LONG slot = InterlockedIncrement(&gHead) - 1;
    const uint8_t phase = GetCurrentThreadId() == ObjectHooks::gGameThread ? ObjectHooks::gPhase : (uint8_t)(10 + ObjectHooks::gPhase);
    if (slot - gTail >= kRing) InterlockedIncrement(&gDropped);
    else {
        Event& e = gRing[slot % kRing];
        e = {obj, *reinterpret_cast<uint32_t*>(obj + 4), *reinterpret_cast<uint32_t*>(obj), *reinterpret_cast<uint32_t*>(obj + 8), false,
             phase, caller, {}};
        // Who killed it (soak 9607: a particle died in a replay's first frame only; the dtor 'from' says nothing).
        if (Lab::On()) RtlCaptureStackBackTrace(1, 6, reinterpret_cast<PVOID*>(e.stack), nullptr);
    }
    InterlockedIncrement(&gDestroyedSinceMark);
    InterlockedDecrement(&gInFlight);
}
}

namespace ObjectHooks {
volatile uint8_t gPhase = 0;
volatile uint32_t gGameThread = 0;
}

#if defined(_MSC_VER) && !defined(__clang__)
__declspec(naked) static void HkCtor() {
    __asm {
        pushfd
        pushad
        push dword ptr [esp+36]
        push ecx
        call MK9R_OnCtor
        add esp, 8
        popad
        popfd
        jmp dword ptr [gMK9R_CtorTramp]
    }
}
__declspec(naked) static void HkDtor() {
    __asm {
        pushfd
        pushad
        push dword ptr [esp+36]
        push ecx
        call MK9R_OnDtor
        add esp, 8
        popad
        popfd
        mov dword ptr [ecx], 0x00C66B08
        mov dword ptr ds:[0x00E394E8], 1
        mov dword ptr [ecx+4], 0xFFFFFFFF
        ret
    }
}
#else
extern "C" void MK9R_HkCtor();
extern "C" void MK9R_HkDtor();
asm(R"(
    .text
    .globl _MK9R_HkCtor
_MK9R_HkCtor:
    pushfl
    pushal
    pushl 36(%esp)
    pushl %ecx
    call _MK9R_OnCtor
    addl $8, %esp
    popal
    popfl
    jmp *_gMK9R_CtorTramp

    .globl _MK9R_HkDtor
_MK9R_HkDtor:
    pushfl
    pushal
    pushl 36(%esp)
    pushl %ecx
    call _MK9R_OnDtor
    addl $8, %esp
    popal
    popfl
    movl $0x00C66B08, (%ecx)
    movl $1, 0x00E394E8
    movl $0xFFFFFFFF, 4(%ecx)
    ret
)");
static void (*const HkCtor)() = MK9R_HkCtor;
static void (*const HkDtor)() = MK9R_HkDtor;
#endif

namespace {
// Type name via the object's vt[0], a leaf "mov eax, imm32; ret" returning the
// type descriptor {char* name, u32 size, desc* parent} (docs/state_ownership.md).
const char* TypeName(uint32_t vtable) {
    static const char* unknown = "?";
    if (vtable < 0x00401000 || vtable >= 0x010AE000) return unknown;
    const uint32_t fn = *reinterpret_cast<const uint32_t*>(vtable);
    if (fn < 0x00401000 || fn >= 0x00F9C000) return unknown;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(fn);
    if (p[0] != 0xB8 || p[5] != 0xC3) return unknown;
    uint32_t desc;
    memcpy(&desc, p + 1, 4);
    if (desc < 0x00401000 || desc >= 0x010AE000) return unknown;
    // Full chain "Type<Parent<...<MKBaseObj" via descriptor parent links (+8), cached per descriptor.
    static std::unordered_map<uint32_t, std::string> cache;
    auto it = cache.find(desc);
    if (it != cache.end()) return it->second.c_str();
    std::string chain;
    for (uint32_t d = desc; d >= 0x00401000 && d < 0x010AE000 && chain.size() < 200;
         d = *reinterpret_cast<const uint32_t*>(d + 8)) {
        const uint32_t name = *reinterpret_cast<const uint32_t*>(d);
        if (name < 0x00401000 || name >= 0x010AE000) break;
        if (!chain.empty()) chain += '<';
        chain += reinterpret_cast<const char*>(name);
    }
    return (cache[desc] = chain.empty() ? std::string("?") : chain).c_str();
}

uint8_t gJmpCtor[8], gJmpDtor[6];
bool gInstalled = false;
}  // namespace

namespace ObjectHooks {

bool Install() {
    uint8_t* tramp = static_cast<uint8_t*>(CodePatch::AllocExec(16));
    if (!tramp) return false;
    memcpy(tramp, kCtorOrig, 8);  // relocation-free: absolute operand only
    tramp[8] = 0xE9;
    const int32_t back = (int32_t)((kCtor + 8) - (reinterpret_cast<uintptr_t>(tramp) + 13));
    memcpy(tramp + 9, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    gMK9R_CtorTramp = tramp;

    auto jmp = [](uint8_t* out, uint32_t len, uintptr_t from, void (*to)()) {
        memset(out, 0x90, len);
        out[0] = 0xE9;
        const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(to) - (from + 5));
        memcpy(out + 1, &rel, 4);
    };
    jmp(gJmpCtor, 8, kCtor, HkCtor);
    jmp(gJmpDtor, 6, kDtor, HkDtor);
    if (!CodePatch::Write(kDtor, kDtorOrig, gJmpDtor, 6, "MKBaseObj::dtor")) return false;
    if (!CodePatch::Write(kCtor, kCtorOrig, gJmpCtor, 8, "MKBaseObj::ctor")) {
        // Only the destructor patch: RestoreAll also removed Safe mode's patches (script clock and RNG, the pause guard,
        // the out-of-memory hook), whose modules never re-applied them.
        CodePatch::Restore(kDtor);
        return false;
    }
    gInstalled = true;
    return true;
}

void Uninstall() {
    CodePatch::Restore(kCtor);
    CodePatch::Restore(kDtor);
    gInstalled = false;
}

long InFlight() { return gInFlight; }

void FlushFrame(uint32_t gameFrame) {
    const LONG head = gHead;
    int created = 0, destroyed = 0;
    for (; gTail < head; ++gTail) {
        const Event e = gRing[gTail % kRing];
        if (e.created) {
            // Constructor chain has completed by end of tick: derived vtable, id and actor are set.
            const uint32_t vt = *reinterpret_cast<const uint32_t*>(e.obj);
            const char* type = TypeName(vt);
            const uint32_t id = *reinterpret_cast<const uint32_t*>(e.obj + 4);
            if (id == 0xFFFFFFFF) {
                if (Lab::On()) Logger::Frame("OBJ+- %08X created and destroyed within the tick", e.obj);
            } else {
                gLive[e.obj] = {type, gameFrame};
                if (gTracking) gHistory.push_back({gameFrame, e.obj, id, type, true, 0});
                if (Lab::On()) Logger::Frame("OBJ+%s gf=%u obj=%08X id=%08X type=%s actor=%08X ph=%u from=%08X stk=%08X %08X %08X %08X %08X %08X", Rollback::gIsResimulating ? "R" : "", gameFrame, e.obj, id, type,
                              *reinterpret_cast<const uint32_t*>(e.obj + 8), e.phase, e.caller, e.stack[0], e.stack[1], e.stack[2], e.stack[3],
                              e.stack[4], e.stack[5]);
            }
            ++created;
        } else {
            auto it = gLive.find(e.obj);
            const char* type = it != gLive.end() ? it->second.first : TypeName(e.vtable);
            const uint32_t born = it != gLive.end() ? it->second.second : 0;
            if (Lab::On()) Logger::Frame("OBJ-%s gf=%u obj=%08X id=%08X type=%s age=%d ph=%u from=%08X stk=%08X %08X %08X %08X %08X %08X",
                          Rollback::gIsResimulating ? "R" : "", gameFrame, e.obj, e.id, type, born ? (int)(gameFrame - born) : -1, e.phase,
                          e.caller, e.stack[0], e.stack[1], e.stack[2], e.stack[3], e.stack[4], e.stack[5]);
            if (it != gLive.end()) gLive.erase(it);
            if (gTracking) gHistory.push_back({gameFrame, e.obj, e.id, type, false, e.actor});
            ++destroyed;
        }
    }
    if (gDropped) {
        Logger::Frame("OBJ ring overflow: %ld events dropped", gDropped);
        gDropped = 0;
    }
    (void)created;
    (void)destroyed;
}

uint32_t BirthGf(uint32_t obj) {
    auto it = gLive.find(obj);
    if (it != gLive.end()) return it->second.second;
    // Not flushed yet: created in this very frame?
    for (LONG i = gTail; i < gHead; ++i) {
        const Event& e = gRing[i % kRing];
        if (e.created && e.obj == obj)
            return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70);  // GameFrame
    }
    return 0;  // unknown: created before tracking began (treated as long-lived)
}

uint32_t CurrentGf() { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70); }

void StartTracking(uint32_t gf) {
    if (!gTracking) {
        gTracking = gInstalled;
        gTrackingSinceGf = gf;
        gHistory.clear();
    }
}

void RestartTracking(uint32_t gf) {
    gTracking = gInstalled;
    gTrackingSinceGf = gf;
    gHistory.clear();
}

void TrimHistory(uint32_t oldestNeededGf) {
    size_t keep = 0;
    while (keep < gHistory.size() && gHistory[keep].gf < oldestNeededGf) ++keep;
    gHistory.erase(gHistory.begin(), gHistory.begin() + keep);
}

namespace {
struct LoadSets {
    std::vector<HistEvent> bornAlive;     // born at gf >= S, still alive now
    std::vector<HistEvent> diedExisting;  // died at gf >= S, not born at gf >= S
};
LoadSets ComputeSets(uint32_t S) {
    LoadSets sets;
    std::unordered_map<uint32_t, size_t> bornIdx;  // obj -> index into bornAlive (latest birth)
    for (const HistEvent& e : gHistory) {
        if (e.gf < S) continue;
        if (e.born) {
            bornIdx[e.obj] = sets.bornAlive.size();
            sets.bornAlive.push_back(e);
        } else {
            auto it = bornIdx.find(e.obj);
            if (it != bornIdx.end() && sets.bornAlive[it->second].id == e.id) {
                sets.bornAlive[it->second].obj = 0;  // born and died after S: nothing to do
                bornIdx.erase(it);
            } else {
                sets.diedExisting.push_back(e);
            }
        }
    }
    sets.bornAlive.erase(std::remove_if(sets.bornAlive.begin(), sets.bornAlive.end(),
                                        [](const HistEvent& e) { return e.obj == 0; }),
                         sets.bornAlive.end());
    return sets;
}
using DeletingDtor = void*(__thiscall*)(void* self, int flags);
}  // namespace

bool CanLoadTo(uint32_t S, char* why, size_t whyLen) {
    FlushFrame(CurrentGf());
    if (!gTracking || S < gTrackingSinceGf) {
        snprintf(why, whyLen, "object history does not cover frame %u", S);
        return false;
    }
    // An MKObj that existed at S and died since comes back with the heap image. Its UE3 actor
    // must still exist: either it had none, or ActorDefer kept it alive (DestroyActor deferred).
    // Anything else (an actor destroyed for real, or un-rooted/un-referenced by a non-owner)
    // makes the rollback refuse. (Post-restore retirement broke the restored lists, run47.)
    for (const HistEvent& e : ComputeSets(S).diedExisting) {
        if (!IsMKObj(e.type) || e.actor == 0 || ActorDefer::Deferred(e.obj, e.id)) continue;
        snprintf(why, whyLen, "actor-owning %s (obj %08X, actor %08X) died since frame %u", e.type, e.obj, e.actor, S);
        return false;
    }
    return true;
}

// Steps 1 and 3 of a churn-aware load (CanLoadTo must have passed).
// PreRestore: destroy actor-owning objects born since S (proper game teardown incl. actor;
// the caller sets ActorDefer bypass so those actors really go).
// PostRestore: drop the abandoned timeline's history. Objects that died since S are back with
// the heap image and their actors come back through ActorDefer::OnLoad. (A former "retire
// resurrected effects" step deleted them again right after the load and broke the restored
// tick/unlink lists, run47.)
int PreRestore(uint32_t S) {
    LoadSets sets = ComputeSets(S);
    int n = 0;
    // Only the abandoned timeline's own objects take their actors with them for real.
    static std::vector<uint32_t> owners;
    owners.clear();
    for (const HistEvent& e : sets.bornAlive) owners.push_back(e.obj);
    ActorDefer::SetBypassOwners(&owners);
    for (const HistEvent& e : sets.bornAlive) {
        if (!IsMKObj(e.type)) continue;                                     // pure MK objects vanish with the heap image
        if (*reinterpret_cast<const uint32_t*>(e.obj + 4) != e.id) continue;  // already gone
        const uint32_t dtor = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(e.obj) + 4);
        Logger::Frame("ROLLBACK destroy post-snapshot %s %08X id=%08X via vt[1]=%08X", e.type, e.obj, e.id, dtor);
        reinterpret_cast<DeletingDtor>(dtor)(reinterpret_cast<void*>(e.obj), 1);
        ++n;
    }
    ActorDefer::SetBypassOwners(nullptr);
    FlushFrame(CurrentGf());
    return n;
}

int PostRestore(uint32_t S) {
    FlushFrame(CurrentGf());
    // Everything stamped >= S belongs to the abandoned timeline (including our cleanup).
    while (!gHistory.empty() && gHistory.back().gf >= S) gHistory.pop_back();
    return 0;
}
// ---- single-slot compatibility (saveab/loadab) ----
void Mark() {
    InterlockedExchange(&gCreatedSinceMark, 0);
    InterlockedExchange(&gDestroyedSinceMark, 0);
    gMarkGf = CurrentGf();
    StartTracking(gMarkGf);
    TrimHistory(gMarkGf);
}
bool CanLoad(char* why, size_t whyLen) { return CanLoadTo(gMarkGf, why, whyLen); }
int DestroyBornSinceMark() { return PreRestore(gMarkGf); }
int RetireResurrected() { return PostRestore(gMarkGf); }

bool Installed() { return gInstalled; }

void ForEachLive(void (*fn)(uint32_t obj, const char* type, void* ctx), void* ctx) {
    for (auto& [obj, v] : gLive)
        if (*reinterpret_cast<const uint32_t*>(obj + 4) != 0xFFFFFFFF) fn(obj, v.first, ctx);
}

const char* TypeOf(uint32_t obj) { return TypeName(*reinterpret_cast<const uint32_t*>(obj)); }

void ForEachListed(void (*fn)(uint32_t obj, const char* type, void* ctx), void* ctx) {
    // Global MK object list [0x00DE8214]: nodes {vt, obj +4, id +8, next +0xC, ...}.
    int guard = 0;
    for (uint32_t node = *reinterpret_cast<const uint32_t*>(0x00DE8214); node && guard < 20000;
         node = *reinterpret_cast<const uint32_t*>(node + 0xC), ++guard) {
        if (node < 0x01000000 || (node & 3)) break;
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(node + 4);
        const uint32_t id = *reinterpret_cast<const uint32_t*>(node + 8);
        if (!obj || *reinterpret_cast<const uint32_t*>(node + 0x18) || *reinterpret_cast<const uint32_t*>(obj + 4) != id)
            continue;
        fn(obj, TypeOf(obj), ctx);
    }
}
uint32_t CreatedSinceMark() { return (uint32_t)gCreatedSinceMark; }
uint32_t DestroyedSinceMark() { return (uint32_t)gDestroyedSinceMark; }

}  // namespace ObjectHooks





