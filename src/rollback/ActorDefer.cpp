#include "ActorDefer.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../common/StrAppend.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/GameLoopHook.h"
#include "../hook/ObjectHooks.h"

namespace {
constexpr uintptr_t kCallSite = 0x00439A70;      // call GWorld->DestroyActor in the MKObj destructor
constexpr uintptr_t kDestroyActor = 0x0079D960;  // UWorld::DestroyActor(actor, bNetForce, bShouldModifyLevel), ret 0xC
constexpr uintptr_t kGWorld = 0x00E86AB0;
constexpr uint32_t kRefOffset = 0x9C;            // actor -> MK object reference {vt, obj, id}
constexpr uint32_t kRootSet = 0x4000;            // RF_RootSet in actor+8 (cleared by 0x005EDAB0)

using DestroyActorFn = int(__thiscall*)(void* world, void* actor, int netForce, int modifyLevel);
const DestroyActorFn gDestroyActor = reinterpret_cast<DestroyActorFn>(kDestroyActor);

struct Entry {
    uint32_t world;  // GWorld at deferral: a level change takes the actor with it
    uint32_t actor, vtable, nameIndex;
    uint32_t mkobj, mkid;
    uint32_t bornGf;  // owner's creation frame (ObjectHooks), 0 = long-lived
    uint8_t ref[12];
    bool rooted;
    uint32_t gf;
};
std::vector<Entry> gList;  // protected by gLock
CRITICAL_SECTION gLock;
bool gLockInit = false;
bool gInstalled = false;
volatile bool gActive = false, gBypass = false;
volatile uint32_t gFrame = 0;
const std::vector<uint32_t>* gBypassOwners = nullptr;  // game thread only (rollback PreRestore)

uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

bool Readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (p < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// The actor is still the same live object we deferred (not destroyed or reused meanwhile).
bool StillOurs(const Entry& e) {
    // Same live object, and not destroyed meanwhile: AActor flags +0x80, bit 0x20 = bDeleteMe
    // (UWorld::DestroyActor 0x0079D97E returns early on it). A deferred particle actor can destroy
    // itself while its owner is dead; giving it back crashed a later destroy (soak15, 0x0078663F).
    return Readable(e.actor, kRefOffset + 12) && Read32(e.actor) == e.vtable && Read32(e.actor + 0x28) == e.nameIndex &&
           !(Read32(e.actor + 0x80) & 0x20);
}

// A deferred actor's owner is dead (its destructor ran, only DestroyActor was postponed): the actor's
// back-reference {vt, obj, id} at +0x9C must not name it any more, or the actor's own destruction (by us,
// or by a world teardown after DropAll) writes into the freed MK memory - a node-pool 'free' vtable
// 0x00D4D980 in a master-heap free chunk, 0x0042AF1D at the next fight start (GGPO synctest 2026-10-15).
void DetachOwner(const Entry& e) {
    if (!StillOurs(e)) return;
    *reinterpret_cast<uint32_t*>(e.actor + kRefOffset + 4) = 0;
    *reinterpret_cast<uint32_t*>(e.actor + kRefOffset + 8) = 0xFFFFFFFF;
}

void ReallyDestroy(const Entry& e) {
    const uint32_t world = Read32(kGWorld);
    if (!world || world != e.world || !StillOurs(e)) {
        // Destroying an actor of a previous world crashed in the actor lookup 0x0071C53C at the
        // attract fight transition (soak6): the level teardown owns it now.
        Logger::Frame("ACTORDEFER drop %08X (world %08X, deferred in %08X)", e.actor, world, e.world);
        return;
    }
    gDestroyActor(reinterpret_cast<void*>(world), reinterpret_cast<void*>(e.actor), 0, 1);  // detached at deferral
}

}  // namespace

// Reached through MK9R_HkDestroyActor below: EDX = the dying MK object (the caller's ESI,
// `mov esi, ecx` at 0x00439A16; EDX is free in a thiscall).
extern "C" int __fastcall MK9R_DestroyActorImpl(void* world, uint32_t mkobj, void* actor, int netForce, int modifyLevel) {
    const uint32_t a = reinterpret_cast<uint32_t>(actor);
    bool bypass = gBypass;
    if (gBypassOwners)
        for (uint32_t o : *gBypassOwners)
            if (o == mkobj) bypass = true;
    // Only deaths after the fight body ran in this tick: normal effect deaths (soak7 log: all
    // 269 in phase 2 after the fight update). The end-of-fight teardown runs in a tick without a
    // fight update; deferring it left actors registered in a world being torn down and the next
    // level start crashed in 0x0071C510 (reproduced with no rollback at all, 2026-10-14 09:12).
    if (!GameLoopHook::FoughtThisTick()) bypass = true;
    if (!gActive || bypass || !a || !mkobj || !Readable(a, kRefOffset + 12) || !Readable(mkobj, 0x30))
        return gDestroyActor(world, actor, netForce, modifyLevel);
    Entry e{};
    e.world = reinterpret_cast<uint32_t>(world);
    e.actor = a;
    e.vtable = Read32(a);
    e.nameIndex = Read32(a + 0x28);
    memcpy(e.ref, reinterpret_cast<const void*>(a + kRefOffset), 12);
    e.mkobj = mkobj;
    e.mkid = Read32(mkobj + 4);
    e.bornGf = ObjectHooks::BirthGf(mkobj);
    // 0x005EDAB0 has already cleared RF_RootSet if the owner had rooted the actor (obj+0xC).
    e.rooted = Read32(mkobj + 0xC) != 0;
    e.gf = gFrame;
    Logger::Frame("ACTORDEFER defer actor=%08X mkobj=%08X gf=%u", a, mkobj, gFrame);
    EnterCriticalSection(&gLock);
    gList.push_back(e);
    LeaveCriticalSection(&gLock);
    // Diagnostic: every other place in the actor and its components that names the owner (pointer or id).
    // An actor near the end of a committed region is shorter than the scan: read only what is readable
    // (netplay 2026-10-27: a particle actor 0x260 bytes before an uncommitted page crashed here).
    static int scanned = 0;
    if (scanned < 12) {
        ++scanned;
        char line[400];
        int n = 0;
        uint32_t scanEnd = 0x300;
        while (scanEnd > kRefOffset + 12 && !Readable(a, scanEnd)) scanEnd -= 0x20;
        for (uint32_t o = 0; o + 4 <= scanEnd; o += 4) {
            const uint32_t v = Read32(a + o);
            if (n > (int)sizeof line - 40) break;
            if ((v == mkobj || v == e.mkid) && o != kRefOffset + 4 && o != kRefOffset + 8)
                n = AppendF(line, sizeof line, n, " actor+%X=%s", o, v == mkobj ? "obj" : "id");
        }
        const uint32_t comps = Read32(a + 0x3C), ncomp = Read32(a + 0x40);
        for (uint32_t k = 0; k < ncomp && k < 16 && Readable(comps, ncomp * 4); ++k) {
            const uint32_t c = Read32(comps + 4 * k);
            if (!c || !Readable(c, 0x400)) continue;
            for (uint32_t o = 0; o + 4 <= 0x400; o += 4) {
                const uint32_t v = Read32(c + o);
                if (n > (int)sizeof line - 40) break;
                if (v == mkobj || v == e.mkid) n = AppendF(line, sizeof line, n, " comp%u+%X=%s", k, o, v == mkobj ? "obj" : "id");
            }
        }
        line[n] = 0;
        Logger::Info("ACTORDEFER owner refs of actor %08X (owner %08X id %08X):%s", a, mkobj, e.mkid, n ? line : " none besides +0x9C");
    }
    // The owner's destructor goes on to free it while the actor stays alive (and ticks) until the
    // release: detach the back-reference now (e.ref above holds the bytes for a give-back at load).
    // Left in place, the actor reached the freed owner through it (master-heap free tree holding a
    // node-pool vtable 0x00D4D980, 0x0042AF1D, frames after a deferral; GGPO synctest 2026-10-15).
    DetachOwner(e);
    return 1;
}

extern "C" void MK9R_HkDestroyActor();
asm(R"(
    .text
    .globl _MK9R_HkDestroyActor
_MK9R_HkDestroyActor:
    movl %esi, %edx
    jmp @MK9R_DestroyActorImpl@20
)");

namespace ActorDefer {

bool Install() {
    if (gInstalled) return true;
    if (!gLockInit) {
        InitializeCriticalSection(&gLock);
        gLockInit = true;
    }
    uint8_t expected[5] = {0xE8, 0, 0, 0, 0}, replacement[5] = {0xE8, 0, 0, 0, 0};
    const int32_t relOld = (int32_t)(kDestroyActor - (kCallSite + 5));
    const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkDestroyActor) - (kCallSite + 5));
    memcpy(expected + 1, &relOld, 4);
    memcpy(replacement + 1, &relNew, 4);
    gInstalled = CodePatch::Write(kCallSite, expected, replacement, 5, "MKObj dtor -> UWorld::DestroyActor (deferred)");
    if (!gInstalled) Logger::Info("ACTORDEFER: call site 0x00439A70 not as expected; actor deaths stay irreversible");
    return gInstalled;
}

void SetActive(bool on) { gActive = on && gInstalled; }
void SetFrame(uint32_t gf) { gFrame = gf; }
void SetBypass(bool on) { gBypass = on; }
void SetBypassOwners(const std::vector<uint32_t>* owners) { gBypassOwners = owners; }

void Release(uint32_t oldestNeededGf) {
    if (!gLockInit) return;
    std::vector<Entry> due;
    EnterCriticalSection(&gLock);
    for (size_t i = 0; i < gList.size();) {
        if (gList[i].gf < oldestNeededGf) {
            due.push_back(gList[i]);
            gList.erase(gList.begin() + i);
        } else {
            ++i;
        }
    }
    LeaveCriticalSection(&gLock);
    if (!due.empty()) Logger::Frame("ACTORDEFER release %u (oldest needed gf %u)", (unsigned)due.size(), oldestNeededGf);
    gBypass = true;
    for (const Entry& e : due) ReallyDestroy(e);
    gBypass = false;
}

void ReleaseAll() { Release(0xFFFFFFFF); }

void DropAll() {
    if (!gLockInit) return;
    EnterCriticalSection(&gLock);
    if (!gList.empty()) Logger::Frame("ACTORDEFER drop %u deferred actors at timeline restart", (unsigned)gList.size());
    // Not touched: the world teardown has destroyed (and maybe freed) them by now; their back-references
    // were detached at deferral. Writing into them here put 0/-1 into freed heap chunks (0x0042AF1D).
    gList.clear();
    LeaveCriticalSection(&gLock);
}

int OnLoad(uint32_t S) {
    if (!gLockInit) return 0;
    int n = 0;
    std::vector<Entry> destroy;
    EnterCriticalSection(&gLock);
    for (size_t i = 0; i < gList.size();) {
        const Entry& e = gList[i];
        if (e.gf >= S && e.bornGf >= S && e.bornGf != 0) {
            // The owner did not exist at S: it is not in the restored image (its memory is free
            // there). Giving the actor back would leave it holding a ref into free MK memory
            // (soak12: an MK id written into the object heap's free tree, 0x0042AF1D).
            destroy.push_back(e);
            gList.erase(gList.begin() + i);
            continue;
        }
        if (e.gf >= S) {
            if (StillOurs(e)) {
                memcpy(reinterpret_cast<void*>(e.actor + kRefOffset), e.ref, 12);
                if (e.rooted) *reinterpret_cast<uint32_t*>(e.actor + 8) |= kRootSet;
                ++n;
            } else {
                // The restored owner names an actor that is gone (ActorGuard catches its destructor's reads).
                static int vanished = 0;
                if (++vanished <= 20) {
                    const bool readable = Readable(e.actor, 0x84);
                    Logger::Info("ACTORDEFER load S=%u: deferred actor %08X of %08X (died gf %u) vanished before the load: "
                                 "vt %08X (deferred with %08X), +0x80 %08X", S, e.actor, e.mkobj, e.gf,
                                 readable ? Read32(e.actor) : 0, e.vtable, readable ? Read32(e.actor + 0x80) : 0);
                }
            }
            gList.erase(gList.begin() + i);
        } else {
            ++i;
        }
    }
    LeaveCriticalSection(&gLock);
    gBypass = true;
    for (const Entry& e : destroy) {
        // The owner's memory is free in the restored heap image now. The actor's back-reference
        // {vt, obj, id} at +0x9C still names it, and the destruction path can write through it into
        // a free chunk (master-heap free tree holding MK ids / node-pool vtables, 0x0042AF1D, GGPO
        // synctest 2026-10-15). Detach first: obj = 0, id = -1.
        ReallyDestroy(e);  // its owner reference was detached at deferral
    }
    gBypass = false;
    if (!destroy.empty()) Logger::Frame("ACTORDEFER load S=%u: destroyed %u actors of owners born after S", S, (unsigned)destroy.size());
    return n;
}

bool Deferred(uint32_t obj, uint32_t id) {
    if (!gLockInit) return false;
    EnterCriticalSection(&gLock);
    bool hit = false;
    for (const Entry& e : gList)
        if (e.mkobj == obj && e.mkid == id) {
            hit = StillOurs(e);  // an actor that destroyed itself meanwhile cannot be given back
            break;
        }
    LeaveCriticalSection(&gLock);
    return hit;
}

uint32_t Pending() {
    if (!gLockInit) return 0;
    EnterCriticalSection(&gLock);
    const uint32_t n = (uint32_t)gList.size();
    LeaveCriticalSection(&gLock);
    return n;
}

}  // namespace ActorDefer
