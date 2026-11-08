#include "FreeQuarantine.h"

#include <cstdio>
#include <emmintrin.h>
#include <windows.h>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#include "../debug/CrashLog.h"
#include "../debug/Lab.h"
#include "../debug/Logger.h"
#include "../game/MKHeap.h"
#include "../hook/HookManager.h"
#include "../hook/ObjectHooks.h"
#include "RollbackManager.h"

namespace {
constexpr uintptr_t kGMallocPtr = 0x00E3EFC8;
constexpr uintptr_t kOuterVtable = 0x00C5ABB4;
constexpr uintptr_t kOuterFree = 0x004056A0;
constexpr uintptr_t kInnerVtable = 0x00C5AB58;
constexpr uintptr_t kInnerFree = 0x00405AF0;

using FreeFn = void(__thiscall*)(void* self, void* ptr);
FreeFn gOuterFree = nullptr, gInnerFree = nullptr;
void* gInnerThis = nullptr;

// ptr -> newest frame stamp. Keyed by pointer because a rollback replays frees: a block
// freed at frame f, then referenced again by the restored state and freed again by the
// resimulated frame f, must be freed ONCE (run38: double free -> allocator corruption).
struct PendingFree {
    uint32_t gf;
    void* heap;    // non-null: owner allocator free (thiscall heap->Free(ptr, region)), see kind
    void* region;
    uint8_t kind;  // 0 GMalloc inner free, 1 MK heap free, 2 small-block allocator free
    uint32_t caller;  // return address of the hooked free (diagnostics)
    bool parked;      // released only by ReleaseAll (timeline restart / mode off)
    uint32_t size, hash;  // block size and content hash at free time (written-after-free detector)
    uint32_t allocGf;     // frame the block was allocated in (kNoGf: before tracking / unknown)
};
constexpr uint32_t kNoGf = 0xFFFFFFFF;
std::unordered_map<void*, PendingFree> gList;  // protected by gLock
// Release order: every non-parked entry is listed under its stamp, again whenever the stamp changes. Release takes the
// buckets that are due and frees an entry only if its stamp still matches (a stale listing is skipped), so it touches
// the blocks due instead of walking the whole quarantine every frame (2026-11-03 online: ~200,000 entries, mostly parked,
// walked per frame while the game's frame rate fell to nothing). Protected by gLock.
std::map<uint32_t, std::vector<void*>> gDue;
CRITICAL_SECTION gLock;
bool gLockInit = false;
volatile bool gActive = false;
volatile uint32_t gFrame = 0;
void TrackAlloc(void* p, uint32_t size, uint32_t caller);
uint32_t UntrackAlloc(void* p);
extern volatile DWORD gGameThreadId;
bool gInstalled = false;

// Every free while rollback mode is on (any thread, any phase): a block referenced by a
// loadable snapshot must never be reused, whoever frees it (run36: a between-frame free +
// reuse let a block restore overwrite its new owner).
bool ShouldDefer() { return gActive; }

// Blocks whose contents a snapshot in the window restores (xblocks: anim arrays, channel arrays,
// playback blocks; skeletal bone arrays). Frees of these are deferred on any thread; other frees
// only on the game thread (render/worker-thread and middleware frees keep the allocator's normal
// reuse, which some subsystems rely on). Per ring slot, refcounted. Protected by gWinLock.
constexpr int kWinSlots = 16;
std::vector<uint32_t> gWinSlot[kWinSlots];
std::unordered_map<uint32_t, int> gWinCount;
SRWLOCK gWinLock = SRWLOCK_INIT;
bool InRestoreWindow(void* p) {
    AcquireSRWLockShared(&gWinLock);
    const bool in = gWinCount.count(reinterpret_cast<uint32_t>(p)) != 0;
    ReleaseSRWLockShared(&gWinLock);
    return in;
}
// Game thread; or any thread while a game tick runs (the OpenMP animation workers only run inside
// the engine tick and free blocks the restored image references, e.g. UE3 class-A list nodes;
// soak20 crashed within 6 rollbacks when worker frees passed through); or a restored block.
// Render-thread and other frees between ticks keep the allocator's normal reuse.
// Blocks inside the MK master heap (object heap, pools, sub-heaps) are restored as one image, so
// their frees roll back with it and must not be deferred: a deferred MK-heap free released later
// (in particular by ReleaseAll after the level reset the heap) is a double free into the in-region
// free tree (soak12/22: 0x0042AF1D through the GMalloc free in 0x0053C450).
bool InMasterImage(void* p) {
    const uint32_t m = *reinterpret_cast<const uint32_t*>(0x00ED53F0);
    if (!m) return false;
    const uint32_t base = *reinterpret_cast<const uint32_t*>(m + 0x34), size = *reinterpret_cast<const uint32_t*>(m + 0x24);
    const uint32_t a = reinterpret_cast<uint32_t>(p);
    return a >= base && a < base + size;
}
// Mode 0: defer game-thread / in-tick frees and restored blocks. Mode 1 (window-only): defer only
// the blocks whose contents a snapshot in the window restores; every other free keeps the
// allocator's immediate reuse, which UE3/MK code relies on in many places (decal material
// instances, view structs, middleware objects: stale pointers that "work" because the next
// same-size allocation returns the same address).
// Mode 2 (default): defer frees issued by MK code (object system / heaps / script VM / anim at
// 0x00426000..0x004E0000; fight, AI and script natives at 0x00860000..0x00900000), frees on
// non-game threads during a tick (OpenMP animation workers), and restored blocks. UE3 engine,
// UObject GC, UI/middleware and render frees keep the allocator's immediate reuse. (Window-only,
// mode 1, hung the script VM within 5 rollbacks: MK/script-owned blocks need the deferral.)
volatile int gMode = 0;  // mode 2 crashed a resimulation when UObject GC frees of pending-kill actors passed through (soak25)
bool MkCode(uint32_t c) { return (c >= 0x00426000 && c < 0x004E0000) || (c >= 0x00860000 && c < 0x00900000); }
bool DeferThisFree(void* p, uint32_t effectiveCaller) {
    if (InMasterImage(p)) return false;
    if (InRestoreWindow(p)) return true;
    if (gMode == 1) return false;
    if (gMode == 2)
        return MkCode(effectiveCaller) || (GetCurrentThreadId() != ObjectHooks::gGameThread && ObjectHooks::gPhase != 0);
    return GetCurrentThreadId() == ObjectHooks::gGameThread || ObjectHooks::gPhase != 0;
}

// Frees issued from outside MKKE.exe (PhysXCore.dll frees through the engine allocator: callers
// 0x097175D6/0x0976886B in a live fight) are not game state we restore. They go straight through
// so those modules keep the allocator's immediate-reuse behaviour they may rely on.
bool ForeignCaller(uint32_t caller) { return caller < 0x00401000 || caller >= 0x010AE000; }

// Statically linked middleware (Scaleform GFx, Bink, FaceFX, ... after the CRT at 0x0098C000) frees
// objects through Realloc(p, 0) and keeps using them, relying on the allocator's immediate LIFO
// reuse (soak14: a refcounted 0x298-byte object, vtable 0x00D89FAC, ~20 holders, written after the
// quarantine released it -> small-block free-list corruption 0x007145BD). Its memory is never part
// of the restored state, so its frees go straight through.
bool MiddlewareCaller(uint32_t caller) { return caller >= 0x0098C000 && caller < 0x00C00000; }

// Realloc frees the old block itself (0x00405B40: size 0 -> vt+0x18 returning to 0x00405BCF; move
// -> vt+0x18 returning to 0x00405BA3). Its caller is remembered so those frees are attributed right.
thread_local uint32_t tReallocCaller = 0;
uint32_t EffectiveCaller(uint32_t caller) {
    return (caller == 0x00405BCF || caller == 0x00405BA3) && tReallocCaller ? tReallocCaller : caller;
}
// Engine sites whose freed blocks are written again later (caught by the bin watcher): the deleting
// destructor of class 0x00D110EC (0x0052EC10, bin 0) and the realloc of the global buffer
// [0x00E590E8] (0x005A0FBC, bin 29). Their memory is never part of the restored state.
bool LifoSite(uint32_t c) { return c == 0x0052EC30 || c == 0x005A0FBC; }
bool PassThrough(uint32_t caller) {
    const uint32_t c = EffectiveCaller(caller);
    return ForeignCaller(c) || MiddlewareCaller(c) || LifoSite(c);
}
using ReallocFn = void*(__thiscall*)(void* self, void* ptr, uint32_t size, uint32_t align);
ReallocFn gOuterRealloc = reinterpret_cast<ReallocFn>(0x00405690), gInnerRealloc = reinterpret_cast<ReallocFn>(0x00405B40);
void* __fastcall HkOuterRealloc(void* self, void* /*edx*/, void* ptr, uint32_t size, uint32_t align) {
    const uint32_t prev = tReallocCaller;
    tReallocCaller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    void* r = gOuterRealloc(self, ptr, size, align);
    tReallocCaller = prev;
    if (r != ptr) {
        UntrackAlloc(ptr);
        TrackAlloc(r, size, (uint32_t)(uintptr_t)__builtin_return_address(0));
    }
    return r;
}
void* __fastcall HkInnerRealloc(void* self, void* /*edx*/, void* ptr, uint32_t size, uint32_t align) {
    const uint32_t prev = tReallocCaller;
    if (!prev) tReallocCaller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    void* r = gInnerRealloc(self, ptr, size, align);
    tReallocCaller = prev;
    return r;
}

// Set while this thread performs a real (released) free, so the owner-level hooks below let
// it through instead of deferring it again.
thread_local bool tReleasing = false;

// Blocks of the small-block class 625..704 bytes (bin 29) stay parked until the timeline restarts:
// several subsystems (middleware Realloc(p,0) users, engine view-like structs freed by 0x005A0FBC)
// keep writing such blocks for many frames after freeing them, relying on immediate LIFO reuse.
// Releasing them 12 frames later put them on the free list while still written -> 0x007145BD.
uint32_t BlockSize(void* p) {
    uint32_t info[2] = {0, 0};
    using BlockInfoFn = bool(__cdecl*)(void*, uint32_t*);
    return reinterpret_cast<BlockInfoFn>(0x00405A10)(p, info) ? info[1] : 0;
}
// Small-block classes whose stale users were caught writing released blocks: bin 29 (625..704,
// middleware objects and engine view-like structs) and bin 0 (<= 16, deleting dtor 0x0052EC10 of
// class 0x00D110EC, soak17). Parked for the whole fight; ReleaseAll frees them at the restart.
bool Parked(uint32_t size) { return size > 624 && size <= 704; }  // (parking bin 0 too grew the quarantine to ~600k entries)
// Sites whose bin-29 blocks are not parked (deferred for the usual window only): 0x005B7020 empties the two TArrays of a
// per-frame render structure (Realloc(p, 0) returning to 0x005B7076 / 0x005B709B, GMalloc::Free returning to 0x005B70CF
// / 0x005B70F3) and stores the result back, so nothing keeps the old pointer. On some stages it frees ~17 such blocks a
// frame: parked, they reached ~200,000 entries (~130 MB) in a 3-minute online match (2026-11-03; census top caller
// 0x005B7076 in lag_ns80_d2 too).
bool NoPark(uint32_t caller) { return caller == 0x005B7076 || caller == 0x005B709B || caller == 0x005B70CF || caller == 0x005B70F3; }
void ListDue(void* ptr, const PendingFree& pf) {  // gLock held
    if (!pf.parked) gDue[pf.gf].push_back(ptr);
}
uint32_t HashBlock(const void* p, uint32_t size) {
    // FNV-1a over the first <= 1 KB (the allocator never writes a deferred block: it is still
    // allocated from its point of view, so any change is a write after the game freed it).
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size && i < 1024; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
// Frees made between a load's requeue scan (OnLoad) and ProtectReferenced (gLock): render-thread commands of the abandoned
// timeline run while a load is in progress, and a block they free there was neither scanned by OnLoad nor offered to
// ProtectReferenced, so a cinema restore that put a reference to it back left it to be released while referenced.
bool gCollectLate = false;
std::vector<uint32_t> gLateFrees;
uint32_t gLateProtected = 0;
void Defer(void* ptr, void* heap = nullptr, void* region = nullptr, uint8_t kind = 0, uint32_t caller = 0, uint32_t allocGf = kNoGf) {
    const uint32_t size = BlockSize(ptr);  // block is still allocated here, so the size lookup is safe
    const bool parked = Parked(size) && !NoPark(caller);
    // The hash only feeds the "written after free" report (DoFree): lab only, it read up to 1 KB twice per deferred free.
    const uint32_t hash = size && Lab::On() ? HashBlock(ptr, size) : 0;
    EnterCriticalSection(&gLock);
    auto it = gList.find(ptr);
    bool listed = false;
    if (it == gList.end()) {
        it = gList.emplace(ptr, PendingFree{gFrame, heap, region, kind, caller, parked, size, hash, allocGf}).first;
        ListDue(ptr, it->second);
        listed = true;
    } else if (it->second.gf < gFrame) {
        it->second.gf = gFrame;
        ListDue(ptr, it->second);
        listed = true;
    }
    if (listed && gCollectLate && gLateFrees.size() < 8192) gLateFrees.push_back((uint32_t)(uintptr_t)ptr);
    LeaveCriticalSection(&gLock);
}

// MK master heap [0x00ED53F0] (vtable 0x00C66A3C, Free = vt+4 0x0042B1D0, thiscall(ptr, region),
// ret 8). Its 12 MB region is not in the snapshot, so its header is kept live (not restored)
// and its frees are quarantined like GMalloc frees. The object heap [0x00ED53F4] is restored
// as an image together with its header, so its frees must NOT be deferred.
constexpr uintptr_t kHeapVtable = 0x00C66A3C;
constexpr uintptr_t kHeapFree = 0x0042B1D0;
using HeapFreeFn = void(__thiscall*)(void* self, void* ptr, void* region);
HeapFreeFn gHeapFree = reinterpret_cast<HeapFreeFn>(kHeapFree);

// Diagnostic: the master heap is restored as an image, which is only sound if no other
// thread allocates from or frees to it while a loadable snapshot exists. Count and log
// (first few, with caller) any MK heap traffic off the game thread.
constexpr uintptr_t kHeapAlloc = 0x0042B390;  // thiscall(size, align), ret 8
using HeapAllocFn = void*(__thiscall*)(void* self, uint32_t size, uint32_t align);
HeapAllocFn gHeapAlloc = reinterpret_cast<HeapAllocFn>(kHeapAlloc);
volatile LONG gOffThreadAllocs = 0, gOffThreadFrees = 0;

void NoteOffThread(const char* what, void* self, uint32_t a, void* caller, volatile LONG* counter) {
    if (GetCurrentThreadId() == ObjectHooks::gGameThread || !ObjectHooks::gGameThread) return;
    const LONG n = InterlockedIncrement(counter);
    if (n <= 12)
        Logger::Info("MKHEAP off-thread %s #%ld: heap %p arg %08X caller %p thread %lu gf %u", what, n, self, a, caller,
                     GetCurrentThreadId(), gFrame);
}

// Pressure valve (2026-10-29: MK heap #5 [0xED5400] ran full in the menus with the quarantine on; its allocator
// fails into the fatal handler 0x00406160 at 0x0042B4F5, which never returns, so the failure cannot be caught after
// the call). Before an MK-heap allocation on the game thread: if the heap is nearly full (used +0x28 within 10% or
// 512 KB of the reservation +0x24) and blocks are quarantined, release them all first. A later load may then meet a
// reused block, which beats a certain freeze; it is logged so it shows up in soaks.
volatile LONG gPressureReleases = 0;
uint32_t gPressureGf = 0xFFFFFFFF;
volatile LONG gNearFullLogged = 0;
void PressureCheck(void* self, uint32_t size, void* caller) {
    // Not the master [0xED53F0]: its +0x28 counts the sub-heaps' reservations (menu run 2026-10-29 19:55:
    // 11776/12288 KB "used" while every sub-heap was mostly empty), so it always looks nearly full.
    if (!self || self == *reinterpret_cast<void* const*>(0x00ED53F0)) return;
    uint32_t res = 0, used = 0;
    const uint32_t h = (uint32_t)(uintptr_t)self;
    if (!CrashLog::SafeRead32(h + 0x24, &res) || !CrashLog::SafeRead32(h + 0x28, &used) || !res || used > res) return;
    const uint32_t free = res - used, margin = res / 10 > 0x80000 ? 0x80000 : res / 10;
    if (free >= size + margin) return;
    // Any thread, quarantine on or off: name the heap, the request and the caller before a failure (whose handler
    // never returns) can happen.
    if (InterlockedIncrement(&gNearFullLogged) <= 30) {
        int idx = 0;
        for (int k = 1; k < 5 && !idx; ++k)
            if (*reinterpret_cast<void* const*>(0x00ED53F0 + 4 * k) == self) idx = k + 1;
        Logger::Info("MKHEAP near full: heap #%d %p %u/%u KB used, %u bytes wanted by %p (thread %lu, gf %u, quarantine %s, %u blocks)",
                     idx, self, used >> 10, res >> 10, size, caller, GetCurrentThreadId(), gFrame, gActive ? "on" : "off",
                     FreeQuarantine::Pending());
    }
    if (!gActive || !gLockInit || GetCurrentThreadId() != gGameThreadId) return;
    if (gPressureGf == gFrame) return;  // at most once per frame
    EnterCriticalSection(&gLock);
    const size_t n = gList.size();
    LeaveCriticalSection(&gLock);
    if (!n) return;
    gPressureGf = gFrame;
    FreeQuarantine::ReleaseAll();
    const LONG k = InterlockedIncrement(&gPressureReleases);
    if (k <= 20 || k % 100 == 0)
        Logger::Info("QUARANTINE pressure release #%ld gf=%u: MK heap %p %u/%u KB used, %u bytes wanted; %u quarantined blocks freed",
                     k, gFrame, self, used >> 10, res >> 10, size, (unsigned)n);
}

int ImageHeapIndex(void* self) {  // 1..5 for the MK heaps [0x00ED53F0..0x00ED5400], 0 for other instances (GMalloc's)
    for (int k = 0; k < 5; ++k)
        if (*reinterpret_cast<void* const*>(0x00ED53F0 + 4 * k) == self) return k + 1;
    return 0;
}

// Allocations and frees in MK heap #5 ([0x00ED5400], MK9's online library), for OnlineKeep: a load leaves that heap's
// memory as it is only when nothing was allocated or freed in it since the save. Both go through the vtable hooked
// here (MKKE has no direct call to the free 0x0042B1D0; its one direct call to the alloc, 0x00405459, is GMalloc's
// medium heap).
volatile LONG gHeap5Ops = 0;
bool gHeapAllocHooked = false;
void NoteHeap5(void* self) {
    if (self == *reinterpret_cast<void* const*>(0x00ED5400)) InterlockedIncrement(&gHeap5Ops);
}

void* __fastcall HkHeapAlloc(void* self, void* /*edx*/, uint32_t size, uint32_t align) {
    NoteHeap5(self);
    NoteOffThread("alloc", self, size, __builtin_return_address(0), &gOffThreadAllocs);
    PressureCheck(self, size, __builtin_return_address(0));
    CrashLog::NoteAlloc(self, size, __builtin_return_address(0));
    // 2026-10-31 20:34: MK heap #5 (1 MB, the online menus' allocator 0x0081A4B0) failed to grow at 70 KB used, so the
    // request was large; requests of 256 KB+ on the sub-heaps are logged with their stack (the first 20).
    static volatile LONG bigLogged = 0;
    if (size >= 0x40000 && self != *reinterpret_cast<void* const*>(0x00ED53F0) && ImageHeapIndex(self) &&
        InterlockedIncrement(&bigLogged) <= 20) {
        uint32_t res = 0, used = 0;
        CrashLog::SafeRead32((uintptr_t)self + 0x24, &res);
        CrashLog::SafeRead32((uintptr_t)self + 0x28, &used);
        Logger::Info("MKHEAP large request: heap #%d %u KB wanted (used/reserved %u/%u KB), thread %lu, gf %u", ImageHeapIndex(self),
                     size >> 10, used >> 10, res >> 10, GetCurrentThreadId(), gFrame);
        CrashLog::LogStack("MKHEAP large request");
    }
    return gHeapAlloc(self, size, align);
}

// Off by default: bisect 2026-10-15 (on: crash in 47 rollbacks; off: 0 in 106). Command 'heapdefer 0|1'.
volatile bool gHeapDefer = false;
// The five MK heaps ([0x00ED53F0..0x00ED5400]) live in the master-heap image restored by Tier B.
bool ImageHeap(void* self) {
    for (uintptr_t a = 0x00ED53F0; a <= 0x00ED5400; a += 4)
        if (*reinterpret_cast<void* const*>(a) == self) return true;
    return false;
}

void __fastcall HkHeapFree(void* self, void* /*edx*/, void* ptr, void* region) {
    NoteHeap5(self);
    NoteOffThread("free", self, (uint32_t)(uintptr_t)ptr, __builtin_return_address(0), &gOffThreadFrees);
    // The MK heaps are restored as an image (Tier B "mkmaster"): never deferred. But GMalloc's
    // medium-block heap ([inner+0x30], 0x00E0AC48) is an instance of the same class, and UObject
    // frees reach it directly (appFree 0x0081A460 -> 0x004059C0 -> owner vt+4), bypassing GMalloc's
    // hooked Free. Those frees were immediate, so a restored block (xblock, script module, Tier E
    // object) could be freed in the abandoned timeline and its contents written back over the free
    // tree after a load (0x0042AF1D / 0x0042ACFD / 0x0042AEC2, 2026-10-15). They are deferred like
    // GMalloc frees now.
    const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    // 0x0042AF1D diagnostic: the crashing free tree (object heap) held live MK object ids (0x1000xxxx) as tree links,
    // i.e. a chunk in it was still a live object. Log frees of blocks that are still linked as objects in the global
    // MK object list (0xDE8210: node +4 = object, next +0xC), with the caller chain.
    if (ptr && gActive && Lab::On() && ImageHeap(self) && GetCurrentThreadId() == gGameThreadId) {  // up to 20,000 nodes a free
        static int reported = 0, seen = 0;
        uint32_t node = 0;
        CrashLog::SafeRead32(0x00DE8210 + 4, &node);
        for (int i = 0; node && i < 20000; ++i) {
            uint32_t obj = 0, next = 0;
            if (!CrashLog::SafeRead32(node + 4, &obj) || !CrashLog::SafeRead32(node + 0xC, &next)) break;
            if (obj == (uint32_t)(uintptr_t)ptr) {
                ++seen;
                if (reported++ < 12) {
                    char chain[200];
                    int k = 0;
                    uint32_t fp = (uint32_t)(uintptr_t)__builtin_frame_address(0), ret = 0;
                    for (int d = 0; d < 8; ++d) {
                        uint32_t up = 0;
                        if (!CrashLog::SafeRead32(fp, &up) || up <= fp || !CrashLog::SafeRead32(up + 4, &ret)) break;
                        k += snprintf(chain + k, sizeof chain - k, " %08X", ret);
                        fp = up;
                    }
                    chain[k] = 0;
                    uint32_t id = 0;
                    CrashLog::SafeRead32(obj + 4, &id);
                    Logger::Info("LINKEDFREE #%d gf=%u: heap %p frees %p (id %08X, still in the global MK list as node %08X) caller %08X chain:%s",
                                 seen, gFrame, self, ptr, id, node, caller, chain);
                }
                break;
            }
            node = next;
        }
    }
    // A block whose contents a snapshot restores (script_data/script_module spans, xblocks: GMalloc medium blocks that
    // appFree frees straight into this heap) must never be reusable while a load can write it back: synctest107 crashed
    // at 0x0042ACFD on a free inside a restored script_data span. Deferred even with heapdefer off.
    if (ptr && !ImageHeap(self) && ShouldDefer() && !tReleasing && InRestoreWindow(ptr)) return Defer(ptr, self, region, 1, caller);
    if (ptr && gHeapDefer && !ImageHeap(self) && ShouldDefer() && !tReleasing && !(tReallocCaller && PassThrough(0x00405BCF)) &&
        DeferThisFree(ptr, tReallocCaller ? tReallocCaller : caller))
        return Defer(ptr, self, region, 1, caller);
    gHeapFree(self, ptr, region);
}

// GMalloc small-block allocator (static 0x00E0AC58, vtable 0x00C66ADC): Free = vt+4
// 0x0042CEB0, thiscall(ptr, region), ret 8. GMalloc's inner free (0x00405AF0) and the generic
// free 0x004059C0 (10 call sites that bypass GMalloc's vtable) both route through the page
// map 0x004291A0 to the owner's vt+4, so this is where every small-block free is seen
// (soak 2026-10-14: free-list corruption 0x007145BD with no restore touching free blocks).
constexpr uintptr_t kSbaVtable = 0x00C66ADC;
constexpr uintptr_t kSbaFree = 0x0042CEB0;
HeapFreeFn gSbaFree = reinterpret_cast<HeapFreeFn>(kSbaFree);

void __fastcall HkSbaFree(void* self, void* /*edx*/, void* ptr, void* region) {
    const uint32_t agf = tReleasing ? kNoGf : UntrackAlloc(ptr);
    static volatile LONG logged = 0;
    if (logged < 16 && InterlockedIncrement(&logged) <= 16) {
        // Diagnostic: are the allocator's page descriptors inside the master-heap image?
        MKHeap::Region mr[8];
        const int n = MKHeap::MasterRanges(mr, 8);
        const uintptr_t r = reinterpret_cast<uintptr_t>(region);
        bool inMaster = false;
        for (int i = 0; i < n; ++i) inMaster |= r >= mr[i].base && r < mr[i].base + mr[i].size;
        Logger::Info("SBA free ptr=%p region=%p%s self=%p", ptr, region, inMaster ? " IN-MASTER-IMAGE" : "", self);
    }
    if (ptr && ShouldDefer() && !tReleasing && !(tReallocCaller && PassThrough(0x00405BCF)) &&
        DeferThisFree(ptr, tReallocCaller ? tReallocCaller : (uint32_t)(uintptr_t)__builtin_return_address(0)))
        return Defer(ptr, self, region, 2, (uint32_t)(uintptr_t)__builtin_return_address(0), agf);
    gSbaFree(self, ptr, region);
}

// Diagnostic ring of real frees performed by the quarantine (address, frame stamp, release frame).
struct Released {
    void* ptr;
    uint32_t freedGf, releasedGf, caller;
};
constexpr int kRelRing = 1 << 16;
Released gReleased[kRelRing];
volatile LONG gRelHead = 0;

volatile LONG gWrittenAfterFree = 0;
std::unordered_map<uint32_t, int> gWafCallers;  // game thread (Release)
void DoFree(void* ptr, const PendingFree& p) {
    if (p.size && p.hash && HashBlock(ptr, p.size) != p.hash) {
        InterlockedIncrement(&gWrittenAfterFree);
        if (++gWafCallers[p.caller] <= 2)
            Logger::Info("QUARANTINE written after free: block %p size %u freed at gf %u by %08X (kind %u), released at gf %u",
                         ptr, p.size, p.gf, p.caller, p.kind, gFrame);
    }
    const LONG slot = InterlockedIncrement(&gRelHead) - 1;
    gReleased[slot & (kRelRing - 1)] = {ptr, p.gf, gFrame, p.caller};
    const bool outer = tReleasing;
    tReleasing = true;
    if (p.kind == 2) gSbaFree(p.heap, ptr, p.region);
    else if (p.kind == 1) gHeapFree(p.heap, ptr, p.region);
    else gInnerFree(gInnerThis, ptr);
    tReleasing = outer;
}

// Diagnostic: GMalloc allocations served from inside the MK master-heap image (restored as an
// image: such a block allocated after a save by an owner we do not restore would be handed out
// again after a load). Inner vtable 0x00C5AB58 +0x10 = Malloc 0x004053D0, thiscall(size, align).
using MallocFn = void*(__thiscall*)(void* self, uint32_t size, uint32_t align);
MallocFn gInnerMalloc = reinterpret_cast<MallocFn>(0x004053D0);
volatile LONG gImageAllocs = 0, gImageAllocLogged = 0;
void NoteImageAlloc(void* p, uint32_t size, void* caller);
void* __fastcall HkInnerMalloc(void* self, void* /*edx*/, uint32_t size, uint32_t align) {
    void* p = gInnerMalloc(self, size, align);
    NoteImageAlloc(p, size, __builtin_return_address(0));
    return p;
}
MallocFn gOuterMalloc = reinterpret_cast<MallocFn>(0x00405680);  // outer vt+0x8: ecx+=8; jmp 0x004053D0
void* __fastcall HkOuterMalloc(void* self, void* /*edx*/, uint32_t size, uint32_t align) {
    void* p = gOuterMalloc(self, size, align);
    NoteImageAlloc(p, size, __builtin_return_address(0));
    return p;
}
// Leak measurement: game-thread GMalloc allocations stamped with the frame, removed when the game frees them. At a
// load to S, those made at gf >= S belong to the abandoned timeline; whatever the restored state no longer
// references is leaked (an hour-long soak ran out of address space; memory grew only during GGPO sessions).
struct AllocRec {
    uint32_t gf, size, caller;
};
std::unordered_map<void*, AllocRec> gAllocs;  // protected by gAllocLock
// gAllocs.size(), written under gAllocLock: every GMalloc free on every thread skips the lock while nothing is tracked
// (menus, lockstep, all frees off the game thread outside sessions). A block is tracked before its pointer is returned
// to the game, so a thread freeing it sees the count that includes it.
volatile LONG gAllocCount = 0;
CRITICAL_SECTION gAllocLock;
volatile DWORD gGameThreadId = 0;  // declared above
uint64_t gAbandonedBytes = 0;
uint32_t gAbandonedBlocks = 0, gLoadsSeen = 0;
uint32_t gReclaimed = 0;
uint64_t gReclaimedBytes = 0;
uint32_t gProtected = 0;
uint64_t gProtectedBytes = 0;
double gScanMs = 0;
uint32_t gRequeuedOld = 0;
uint64_t gRequeuedOldBytes = 0;
// The blocks the last OnLoad requeued (older blocks freed after S by non-MK code that no restored span referenced), for
// ProtectReferenced: restores that run after OnLoad (CinemaRestore) can put references to them back.
std::vector<uint32_t> gLastRequeued;
uint32_t gLastRequeuedS = 0xFFFFFFFF;
uint32_t gProtectedLate = 0, gProtectedLateLogged = 0;
bool gProtectLate = true;  // 'protectlate 0|1' (CommandChannel)
volatile bool gUnfreeAll = false;  // old behaviour: every block freed after S is live again after a load ('unfreeall 1')
uint32_t gUnfreed[2] = {0, 0};
uint64_t gUnfreedBytes[2] = {0, 0};
std::unordered_map<uint32_t, uint32_t> gUnfreedBy;
std::unordered_map<uint32_t, std::pair<uint32_t, uint64_t>> gAbandonedBy;  // caller -> (blocks, bytes)
void TrackAlloc(void* p, uint32_t size, uint32_t caller) {
    if (!p || !gActive || !gLockInit || GetCurrentThreadId() != gGameThreadId) return;
    EnterCriticalSection(&gAllocLock);
    gAllocs[p] = {gFrame, size, caller};
    gAllocCount = (LONG)gAllocs.size();
    LeaveCriticalSection(&gAllocLock);
}
uint32_t UntrackAlloc(void* p) {
    if (!p || !gLockInit || !gAllocCount) return kNoGf;
    uint32_t gf = kNoGf;
    EnterCriticalSection(&gAllocLock);
    auto it = gAllocs.find(p);
    if (it != gAllocs.end()) {
        gf = it->second.gf;
        gAllocs.erase(it);
        gAllocCount = (LONG)gAllocs.size();
    }
    LeaveCriticalSection(&gAllocLock);
    return gf;
}
void NoteImageAlloc(void* p, uint32_t size, void* caller) {
    TrackAlloc(p, size, (uint32_t)(uintptr_t)caller);
    const uint32_t m = *reinterpret_cast<const uint32_t*>(0x00ED53F0);
    if (p && m) {
        const uintptr_t base = *reinterpret_cast<const uint32_t*>(m + 0x34), res = *reinterpret_cast<const uint32_t*>(m + 0x24);
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        if (a >= base && a < base + res) {
            InterlockedIncrement(&gImageAllocs);
            if (InterlockedIncrement(&gImageAllocLogged) <= 40)
                Logger::Info("GMALLOC-IN-IMAGE %p size %u caller %p thread %lu gf %u", p, size, caller, GetCurrentThreadId(),
                             gFrame);
        }
    }
}

void __fastcall HkOuterFree(void* self, void* /*edx*/, void* ptr) {
    const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    const uint32_t agf = tReleasing ? kNoGf : UntrackAlloc(ptr);
    if (ptr && ShouldDefer() && !tReleasing && !PassThrough(caller) && DeferThisFree(ptr, EffectiveCaller(caller)))
        return Defer(ptr, nullptr, nullptr, 0, EffectiveCaller(caller), agf);
    const bool outer = tReleasing;
    tReleasing = true;  // the owner-level hook below must not defer what passes through here
    gOuterFree(self, ptr);
    tReleasing = outer;
}
void __fastcall HkInnerFree(void* self, void* /*edx*/, void* ptr) {
    const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    const uint32_t agf = tReleasing ? kNoGf : UntrackAlloc(ptr);
    if (ptr && ShouldDefer() && !tReleasing && !PassThrough(caller) && DeferThisFree(ptr, EffectiveCaller(caller)))
        return Defer(ptr, nullptr, nullptr, 0, EffectiveCaller(caller), agf);
    const bool outer = tReleasing;
    tReleasing = true;
    gInnerFree(self, ptr);
    tReleasing = outer;
}
}  // namespace

namespace FreeQuarantine {

bool Install() {
    if (gInstalled) return true;
    if (!gLockInit) {
        InitializeCriticalSection(&gAllocLock);
        InitializeCriticalSection(&gLock);
        gLockInit = true;
    }
    const uintptr_t gm = *reinterpret_cast<const uint32_t*>(kGMallocPtr);
    if (!gm || *reinterpret_cast<const uint32_t*>(gm) != kOuterVtable) {
        Logger::Info("QUARANTINE: GMalloc %08X has unexpected vtable; refusing", gm);
        return false;
    }
    // Inner allocator vtable 0x00C5AB58 (static analysis + runtime): +0x10 Malloc 0x004053D0,
    // +0x18 Free 0x00405AF0, +0x1C Realloc 0x00405B40 (Realloc frees via +0x18). The outer
    // Free (0x004056A0) jumps straight to 0x00405AF0, so both slots are hooked.
    gInnerThis = reinterpret_cast<void*>(gm + 8);
    const uintptr_t innerVt = *reinterpret_cast<const uint32_t*>(gm + 8);
    if (innerVt != kInnerVtable || *reinterpret_cast<const uint32_t*>(innerVt + 0x18) != kInnerFree) {
        Logger::Info("QUARANTINE: inner allocator vtable %08X unexpected; refusing", innerVt);
        return false;
    }
    gOuterFree = reinterpret_cast<FreeFn>(kOuterFree);
    gInnerFree = reinterpret_cast<FreeFn>(kInnerFree);
    if (!HookManager::PatchPointer(kOuterVtable + 0x10, kOuterFree, reinterpret_cast<void*>(&HkOuterFree),
                                   "GMalloc::Free@outer vt+0x10"))
        return false;
    if (!HookManager::PatchPointer(innerVt + 0x18, kInnerFree, reinterpret_cast<void*>(&HkInnerFree),
                                   "GMalloc inner::Free@vt+0x18")) {
        Logger::Info("QUARANTINE: inner vtable patch failed (vt %08X)", innerVt);
        return false;
    }
    if (!HookManager::PatchPointer(kHeapVtable + 4, kHeapFree, reinterpret_cast<void*>(&HkHeapFree),
                                   "MK heap::Free@vt+4 (master heap only)")) {
        Logger::Info("QUARANTINE: MK heap free hook failed");
        return false;
    }
    if (!HookManager::PatchPointer(innerVt + 0x10, 0x004053D0, reinterpret_cast<void*>(&HkInnerMalloc),
                                   "GMalloc inner::Malloc@vt+0x10 (image diagnostic)") ||
        !HookManager::PatchPointer(kOuterVtable + 0x8, 0x00405680, reinterpret_cast<void*>(&HkOuterMalloc),
                                   "GMalloc::Malloc@outer vt+0x8 (image diagnostic)"))
        Logger::Info("QUARANTINE: malloc diagnostic hook failed (continuing)");
    if (!HookManager::PatchPointer(kOuterVtable + 0xC, 0x00405690, reinterpret_cast<void*>(&HkOuterRealloc), "GMalloc::Realloc@outer vt+0xC") ||
        !HookManager::PatchPointer(innerVt + 0x1C, 0x00405B40, reinterpret_cast<void*>(&HkInnerRealloc), "GMalloc inner::Realloc@vt+0x1C"))
        Logger::Info("QUARANTINE: realloc caller hooks failed (continuing)");
    if (*reinterpret_cast<const uint32_t*>(kSbaVtable + 4) != kSbaFree ||
        !HookManager::PatchPointer(kSbaVtable + 4, kSbaFree, reinterpret_cast<void*>(&HkSbaFree),
                                   "GMalloc small-block allocator::Free@vt+4")) {
        Logger::Info("QUARANTINE: small-block allocator free hook failed; refusing");
        return false;
    }
    gHeapAllocHooked = HookManager::PatchPointer(kHeapVtable + 0x10, kHeapAlloc, reinterpret_cast<void*>(&HkHeapAlloc),
                                                 "MK heap::Alloc@vt+0x10 (off-thread diagnostic, heap #5 count)");
    if (!gHeapAllocHooked) Logger::Info("QUARANTINE: MK heap alloc diagnostic hook failed (continuing)");
    gInstalled = true;
    Logger::Info("QUARANTINE installed (GMalloc %08X, inner vt %08X free %08X)", gm, innerVt, (unsigned)kInnerFree);
    return true;
}

void SetActive(bool on) { gActive = on && gInstalled; }
void SetHeapDefer(bool on) { gHeapDefer = on; }
void SetUnfreeAll(bool on) { gUnfreeAll = on; }
void SetFrame(uint32_t gf) {
    gFrame = gf;
    gGameThreadId = GetCurrentThreadId();  // SetFrame runs on the game thread
}


void Release(uint32_t oldestNeededGf) {
    if (!gLockInit) return;
    if (gAllocCount) {  // allocations older than any loadable frame are ordinary live blocks: stop tracking them
        EnterCriticalSection(&gAllocLock);
        for (auto it = gAllocs.begin(); it != gAllocs.end();)
            it = it->second.gf < oldestNeededGf ? gAllocs.erase(it) : std::next(it);
        gAllocCount = (LONG)gAllocs.size();
        LeaveCriticalSection(&gAllocLock);
    }
    std::vector<std::pair<void*, PendingFree>> toFree;
    EnterCriticalSection(&gLock);
    // gf may wrap backwards on a timeline restart; ReleaseAll handles that.
    for (auto b = gDue.begin(); b != gDue.end() && b->first < oldestNeededGf; b = gDue.erase(b))
        for (void* p : b->second) {
            auto it = gList.find(p);
            if (it == gList.end() || it->second.gf != b->first || it->second.parked) continue;  // released, or listed again
            toFree.emplace_back(it->first, it->second);
            gList.erase(it);
        }
    LeaveCriticalSection(&gLock);
    for (auto& e : toFree) DoFree(e.first, e.second);
}

int OnLoad(uint32_t S, const std::vector<std::pair<uintptr_t, uint32_t>>* restored) {
    if (!gLockInit) return 0;
    {
        // Leak measurement: allocations of the abandoned timeline still not freed by the game.
        EnterCriticalSection(&gAllocLock);
        for (auto it = gAllocs.begin(); it != gAllocs.end();) {
            if (it->second.gf >= S && it->second.gf - S < 64) {
                ++gAbandonedBlocks;
                gAbandonedBytes += it->second.size;
                auto& c = gAbandonedBy[it->second.caller];
                ++c.first;
                c.second += it->second.size;
                it = gAllocs.erase(it);
            } else {
                ++it;
            }
        }
        gAllocCount = (LONG)gAllocs.size();
        LeaveCriticalSection(&gAllocLock);
        if (++gLoadsSeen % 100 == 0) {
            Logger::Info("ALLOCLEAK: %u loads, %u abandoned-timeline allocations not freed (%llu KB); %u tracked live", gLoadsSeen,
                         gAbandonedBlocks, (unsigned long long)(gAbandonedBytes >> 10), (unsigned)gAllocs.size());
            std::vector<std::pair<uint32_t, std::pair<uint32_t, uint64_t>>> top(gAbandonedBy.begin(), gAbandonedBy.end());
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
            for (size_t i = 0; i < top.size() && i < 8; ++i)
                Logger::Info("ALLOCLEAK   caller %08X: %u blocks, %llu KB", top[i].first, top[i].second.first,
                             (unsigned long long)(top[i].second.second >> 10));
        }
    }
    int n = 0;
    gLastRequeued.clear();
    gLastRequeuedS = S;
    std::vector<uint32_t> cand;  // older blocks freed after S by non-MK code: requeued unless restored state points at them
    EnterCriticalSection(&gLock);
    for (auto it = gList.begin(); it != gList.end();) {
        if (it->second.gf >= S) {
            const uint32_t a = it->second.allocGf;
            if (!gUnfreeAll && !(a != kNoGf && a >= S && a - S < 64) && !MkCode(it->second.caller) && !InRestoreWindow(it->first)) {
                cand.push_back((uint32_t)(uintptr_t)it->first);
                ++it;
            } else if (a != kNoGf && a >= S && a - S < 64) {
                // Allocated and freed in the abandoned timeline: the restored state never saw this block, so
                // nothing frees it again. Keep it quarantined as if freed at S and release it normally. (Dropping
                // it leaked every GMalloc block made and freed inside a rolled-back window, including the OS
                // chunks of 256 KB..1 MB allocations: ~150 MB per 600 SyncTest frames, synctest80-90.)
                it->second.gf = S;
                ListDue(it->first, it->second);
                ++gReclaimed;
                gReclaimedBytes += it->second.size;
                ++it;
            } else {  // freed in the abandoned timeline: live again after the load
                const bool mk = MkCode(it->second.caller);
                ++gUnfreed[mk];
                gUnfreedBytes[mk] += it->second.size;
                if (!mk) ++gUnfreedBy[it->second.caller];
                it = gList.erase(it);
                ++n;
            }
        } else {
            ++it;
        }
    }
    gLateFrees.clear();  // from here to ProtectReferenced: frees the scan above did not see
    gCollectLate = true;
    LeaveCriticalSection(&gLock);
    if (!cand.empty()) {
        // Older block freed after S by non-MK code (UE3 engine, containers): its owner is live state the load does
        // not rewind (or a pointer-safe merge kept the owner's new pointer), so nothing frees it again, and
        // un-freeing these leaked ~490 KB per load (synctest94: 147 MB per 300 loads). But releasing one that the
        // restored state still points at is a use-after-free (synctest95: EIP in the heap on a non-game thread).
        // So scan every restored range (live, after the restore) for exact references first.
        LARGE_INTEGER q0, q1, qf;
        QueryPerformanceCounter(&q0);
        uint32_t cap = 64;
        while (cap < cand.size() * 4) cap <<= 1;
        std::vector<uint32_t> set(cap, 0);
        uint32_t lo = 0xFFFFFFFF, hi = 0;
        for (uint32_t p : cand) {
            for (uint32_t i = (p >> 3) * 2654435761u & (cap - 1);; i = (i + 1) & (cap - 1))
                if (!set[i]) {
                    set[i] = p;
                    break;
                }
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
        std::unordered_map<uint32_t, bool> hit;
        auto probe = [&](uint32_t v) {
            for (uint32_t i = (v >> 3) * 2654435761u & (cap - 1); set[i]; i = (i + 1) & (cap - 1))
                if (set[i] == v) {
                    hit[v] = true;
                    break;
                }
        };
        // Four dwords at a time (SSE2): only aligned values inside [lo, hi] are probed (the scan read every restored
        // dword, the 11.8 MB master image included: ~4.2 ms of every load, 2026-11-04).
        const __m128i bias = _mm_set1_epi32((int)0x80000000u), vlo = _mm_set1_epi32((int)lo);
        const __m128i vspan = _mm_set1_epi32((int)((hi - lo) ^ 0x80000000u)), seven = _mm_set1_epi32(7), zero = _mm_setzero_si128();
        if (restored)
            for (auto& [base, size] : *restored) {
                const uint32_t* d = reinterpret_cast<const uint32_t*>(base);
                const uint32_t n = size / 4;
                uint32_t k = 0;
                for (; k + 4 <= n; k += 4) {
                    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + k));
                    const __m128i outside = _mm_cmpgt_epi32(_mm_xor_si128(_mm_sub_epi32(v, vlo), bias), vspan);  // v - lo > hi - lo
                    const __m128i aligned = _mm_cmpeq_epi32(_mm_and_si128(v, seven), zero);
                    const int m = _mm_movemask_ps(_mm_castsi128_ps(_mm_andnot_si128(outside, aligned)));
                    if (!m) continue;
                    for (int j = 0; j < 4; ++j)
                        if (m & (1 << j)) probe(d[k + j]);
                }
                for (; k < n; ++k) {
                    const uint32_t v = d[k];
                    if (v >= lo && v <= hi && !(v & 7)) probe(v);
                }
            }
        QueryPerformanceCounter(&q1);
        QueryPerformanceFrequency(&qf);
        gScanMs += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
        EnterCriticalSection(&gLock);
        for (uint32_t p : cand) {
            auto it = gList.find(reinterpret_cast<void*>((uintptr_t)p));
            if (it == gList.end()) continue;
            if (!restored || hit.count(p)) {  // still referenced (or no ranges given): live again, as before
                ++gProtected;
                gProtectedBytes += it->second.size;
                gList.erase(it);
                ++n;
            } else {
                it->second.gf = S;
                ListDue(it->first, it->second);
                ++gRequeuedOld;
                gRequeuedOldBytes += it->second.size;
                gLastRequeued.push_back(p);
            }
        }
        LeaveCriticalSection(&gLock);
    }
    if (gLoadsSeen % 100 == 0) {
        Logger::Info("ALLOCLEAK: requeue scan %.2f ms/load; %u referenced by restored state kept live (%llu KB)", gScanMs / 100,
                     gProtected, (unsigned long long)(gProtectedBytes >> 10));
        gScanMs = 0;
        Logger::Info("ALLOCLEAK: %u blocks (%llu KB) allocated and freed in abandoned timelines requeued for release", gReclaimed,
                     (unsigned long long)(gReclaimedBytes >> 10));
        Logger::Info("ALLOCLEAK: un-freed at load (older blocks freed after S): MK callers %u (%llu KB), others %u (%llu KB); "
                     "requeued (non-MK, outside the restore window) %u (%llu KB)",
                     gUnfreed[1], (unsigned long long)(gUnfreedBytes[1] >> 10), gUnfreed[0],
                     (unsigned long long)(gUnfreedBytes[0] >> 10), gRequeuedOld, (unsigned long long)(gRequeuedOldBytes >> 10));
        std::vector<std::pair<uint32_t, uint32_t>> top;
        for (auto& [c, k] : gUnfreedBy) top.push_back({k, c});
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        char line[400];
        int m = 0;
        for (size_t i = 0; i < top.size() && i < 10; ++i) m += snprintf(line + m, sizeof line - m, " %08X x%u", top[i].second, top[i].first);
        line[m] = 0;
        Logger::Info("ALLOCLEAK:   non-MK un-free callers:%s", line);
    }
    return n;
}

int ProtectReferenced(uint32_t S, const std::vector<std::pair<uintptr_t, uint32_t>>& ranges, const char* who) {
    if (!gLockInit) return 0;
    std::vector<uint32_t> late;  // freed during the load, after OnLoad's scan (render-thread commands of the abandoned timeline)
    EnterCriticalSection(&gLock);
    late.swap(gLateFrees);
    gCollectLate = false;
    LeaveCriticalSection(&gLock);
    if (!gProtectLate || gLastRequeuedS != S || (gLastRequeued.empty() && late.empty()) || ranges.empty()) return 0;
    std::sort(late.begin(), late.end());
    late.erase(std::unique(late.begin(), late.end()), late.end());
    std::vector<uint32_t> req = gLastRequeued;
    req.insert(req.end(), late.begin(), late.end());
    std::sort(req.begin(), req.end());
    req.erase(std::unique(req.begin(), req.end()), req.end());
    const uint32_t lo = req.front(), hi = req.back();
    struct Hit {
        uint32_t block, where;
    };
    std::vector<Hit> hits;
    for (const auto& [base, size] : ranges) {
        if (!base || size < 4) continue;
        for (uint32_t k = 0; k + 4 <= size; k += 4) {
            uint32_t v = 0;
            if (!CrashLog::SafeRead32(base + k, &v) || v < lo || v > hi || (v & 7)) continue;
            if (std::binary_search(req.begin(), req.end(), v)) hits.push_back({v, (uint32_t)(base + k)});
        }
    }
    if (hits.empty()) return 0;
    struct Kept {
        uint32_t block, where, caller, allocGf, size;
        bool late;
    };
    std::vector<Kept> kept;
    EnterCriticalSection(&gLock);
    for (const Hit& h : hits) {
        auto it = gList.find(reinterpret_cast<void*>((uintptr_t)h.block));
        if (it == gList.end()) continue;  // released already, or made live by an earlier hit
        // A requeued block carries S; a block freed during the load carries the frame it was freed at (>= S). Any other
        // stamp means it was freed again since.
        const bool isLate = std::binary_search(late.begin(), late.end(), h.block) && it->second.gf >= S;
        if (it->second.gf != S && !isLate) continue;
        kept.push_back({h.block, h.where, it->second.caller, it->second.allocGf, it->second.size, isLate});
        gList.erase(it);  // live again: the restored state points at it (its gDue listing is skipped as stale)
    }
    LeaveCriticalSection(&gLock);
    gProtectedLate += (uint32_t)kept.size();
    for (const Kept& k : kept) {
        gLateProtected += k.late;
        if (gProtectedLateLogged++ < 40 || (k.late && gLateProtected <= 20))
            Logger::Info("QUARANTINE load S=%u: block %08X (%u bytes, freed by %08X after S%s, allocated gf %d) is referenced from %08X "
                         "restored by %s after the requeue scan: live again (%u so far)",
                         S, k.block, k.size, k.caller, k.late ? ", during the load" : "", k.allocGf == kNoGf ? -1 : (int)k.allocGf,
                         k.where, who, gProtectedLate);
    }
    return (int)kept.size();
}

uint32_t LateProtected() { return gLateProtected; }

void SetProtectLate(bool on) {
    gProtectLate = on;
    Logger::Info("PROTECTLATE %d: blocks requeued at a load %s", on ? 1 : 0,
                 on ? "stay live when a later restore (cinema) puts a reference to them back" : "are released whatever later restores put back");
}

void ReleaseAll() {
    if (!gLockInit) return;
    EnterCriticalSection(&gAllocLock);
    gAllocs.clear();
    gAllocCount = 0;
    LeaveCriticalSection(&gAllocLock);
    std::unordered_map<void*, PendingFree> all;
    EnterCriticalSection(&gLock);
    all.swap(gList);
    gDue.clear();
    LeaveCriticalSection(&gLock);
    for (auto& e : all) DoFree(e.first, e.second);
}

void Uninstall() {
    gActive = false;
    ReleaseAll();  // HookManager::RemoveAll restores the vtable slots
    gInstalled = gHeapAllocHooked = false;
}

bool Heap5Ops(uint32_t* ops) {
    *ops = (uint32_t)gHeap5Ops;
    return gInstalled && gHeapAllocHooked;
}

uint32_t Pending() {
    if (!gLockInit) return 0;
    EnterCriticalSection(&gLock);
    const uint32_t n = (uint32_t)gList.size();
    LeaveCriticalSection(&gLock);
    return n;
}

bool IsPending(const void* p) {
    if (!gLockInit) return false;
    EnterCriticalSection(&gLock);
    const bool in = gList.count(const_cast<void*>(p)) != 0;
    LeaveCriticalSection(&gLock);
    return in;
}

bool FreedBefore(const void* p, uint32_t S, uint32_t* freedGf) {
    // Held blocks only. rc9 also took a released block whose last release record said "freed before S", but the
    // allocator hands released blocks straight back out: a per-frame TArray realloc got its own block back, the save
    // at S held it live, the game freed it again after S and the load made it live again (no longer listed), so the
    // record of its previous life matched. Batch 21: Jade_XRAY_0 / Main_Sequence_0 +0xD8 "freed before S" on about a
    // third of the X-ray loads, live (abandoned-timeline) headers kept, and two new X-ray crashes.
    if (!gLockInit || !p) return false;
    EnterCriticalSection(&gLock);
    auto it = gList.find(const_cast<void*>(p));
    const bool before = it != gList.end() && it->second.gf < S;
    if (before && freedGf) *freedGf = it->second.gf;
    LeaveCriticalSection(&gLock);
    return before;
}

uint64_t PendingBytesApprox() {  // sizes known at free time (BlockSize; 0 for blocks it cannot size)
    if (!gLockInit) return 0;
    uint64_t b = 0;
    EnterCriticalSection(&gLock);
    for (auto& [ptr, pf] : gList) b += pf.size;
    LeaveCriticalSection(&gLock);
    return b;
}

// What the quarantine holds and why it is still held (2026-10-29: 8,027 blocks and +390 MB private after five
// minutes of menus with the quarantine on). Release(F-11) frees non-parked entries stamped before F-11.
void LogCensus() {
    if (!gLockInit) return;
    const uint32_t F = gFrame;
    uint32_t n = 0, parked = 0, future = 0, recent = 0, unsized = 0, kinds[3] = {0, 0, 0}, oldest = 0xFFFFFFFF, newest = 0;
    uint64_t bytes = 0;
    std::unordered_map<uint32_t, int> callers, parkedBy;
    EnterCriticalSection(&gLock);
    for (auto& [ptr, pf] : gList) {
        ++n;
        if (pf.parked) {
            ++parked;
            ++parkedBy[pf.caller];
        }
        if (pf.gf > F) ++future;
        else if (F - pf.gf <= 11) ++recent;
        if (!pf.size) ++unsized;
        if (pf.kind < 3) ++kinds[pf.kind];
        if (pf.gf < oldest) oldest = pf.gf;
        if (pf.gf > newest) newest = pf.gf;
        bytes += pf.size;
        ++callers[pf.caller];
    }
    LeaveCriticalSection(&gLock);
    int top = 0, topParked = 0;
    uint32_t topCaller = 0, topParkedCaller = 0;
    for (auto& [c, k] : callers)
        if (k > top) top = k, topCaller = c;
    for (auto& [c, k] : parkedBy)
        if (k > topParked) topParked = k, topParkedCaller = c;
    Logger::Info("QUARANTINE census gf=%u: %u blocks (%llu KB sized, %u unsized); parked %u, stamped after now %u, within 11 frames %u; "
                 "kinds gmalloc %u / mkheap %u / sba %u; stamps %u..%u; top caller %08X x%d, top parked caller %08X x%d",
                 F, n, (unsigned long long)(bytes >> 10), unsized, parked, future, recent, kinds[0], kinds[1], kinds[2],
                 oldest == 0xFFFFFFFF ? 0 : oldest, newest, topCaller, top, topParkedCaller, topParked);
}

void LogTopCallers(int n) {
    if (!gLockInit) return;
    std::unordered_map<uint32_t, int> counts;
    EnterCriticalSection(&gLock);
    for (auto& [ptr, pf] : gList) ++counts[pf.caller];
    LeaveCriticalSection(&gLock);
    std::vector<std::pair<int, uint32_t>> v;
    for (auto& [c, k] : counts) v.push_back({k, c});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    Logger::Info("FREECALLERS %u pending, %u distinct callers", Pending(), (unsigned)v.size());
    for (int i = 0; i < n && i < (int)v.size(); ++i) Logger::Info("FREECALLERS %08X x%d", v[i].second, v[i].first);
}

void SetRestoreBlocks(int slot, const std::vector<uint32_t>& blocks) {
    if (slot < 0 || slot >= kWinSlots) return;
    AcquireSRWLockExclusive(&gWinLock);
    for (uint32_t b : gWinSlot[slot]) {
        auto it = gWinCount.find(b);
        if (it != gWinCount.end() && --it->second <= 0) gWinCount.erase(it);
    }
    gWinSlot[slot] = blocks;
    for (uint32_t b : blocks) ++gWinCount[b];
    ReleaseSRWLockExclusive(&gWinLock);
}

void SetMode(int mode) { gMode = mode; Logger::Info("QUARANTINE mode %d", mode); }

void ClearRestoreBlocks() {
    AcquireSRWLockExclusive(&gWinLock);
    for (auto& v : gWinSlot) v.clear();
    gWinCount.clear();
    ReleaseSRWLockExclusive(&gWinLock);
}

// First ring entry of the current timeline. Soak 11 (2026-11-05): a SyncTest session started at gf 1507 right after a
// timeline restart whose ReleaseAll had stamped the previous fight's script-module frees with gf 3367-3371; with the
// quarantine off in between, nothing newer shielded them, so every load of the session took the new fight's modules
// (reallocated at the same addresses) for blocks "released after the save" and skipped them: script data kept live,
// NATIVETRACE 004597E0 returning a countdown 4 frames ahead in every replay (and, likely, the Shang Tsung intro-morph
// failure in the same session). Online the next session's own releases normally shield the old entries.
volatile LONG gRelFloor = 0;
void NewTimeline() { gRelFloor = gRelHead; }

bool RecentlyReleased(const void* p, uint32_t span, uint32_t* freedGf, uint32_t* releasedGf, uint32_t* caller, uint32_t* block,
                      uint32_t minReleasedGf) {
    const LONG head = gRelHead;
    const LONG floor = minReleasedGf ? gRelFloor : 0;
    for (LONG i = head - 1; i >= floor && i >= head - kRelRing; --i) {
        const Released& r = gReleased[i & (kRelRing - 1)];
        if (r.releasedGf < minReleasedGf) break;  // older than asked (65536 entries, ~1 MB, were scanned per call)
        const uintptr_t a = reinterpret_cast<uintptr_t>(r.ptr), q = reinterpret_cast<uintptr_t>(p);
        if (q >= a && q < a + span) {
            *freedGf = r.freedGf;
            *releasedGf = r.releasedGf;
            *caller = r.caller;
            if (block) *block = (uint32_t)a;
            return true;
        }
    }
    return false;
}

}  // namespace FreeQuarantine







