#include "Snapshot.h"

#include <windows.h>
#include <psapi.h>

#include <cstring>

#include "../common/StrAppend.h"
#include "../common/MemQuery.h"
#include "../debug/Logger.h"
#include "../game/MK9Game.h"
#include "../game/MKHeap.h"
#include "StateHash.h"
#include "../debug/CrashLog.h"
#include "../debug/Lab.h"
#include "../debug/Ue3Names.h"
#include "FramePace.h"
#include "FreeQuarantine.h"
#include "OnlineKeep.h"
#include "ParallelMem.h"
#include "RngTrace.h"
#include "ScriptRng.h"
#include "../hook/Ue3Refs.h"
#include "../hook/ObjectHooks.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace {
// Fighter actor = MKPawn: UClass PropertiesSize (UStruct +0x40) is 0x28C (Engine.Pawn 0x27C), in a 0x2C0-byte
// GMalloc slot (verified at runtime 2026-10-17). The span was 0x300 "until the class size is known": it captured
// and restored the first 0x40 bytes of the neighbouring slot, another object or a free block's list link.
constexpr uint32_t kActorSpan = 0x28C;

struct ClockField {
    const char* name;
    uint32_t offset, size;
};
// Filled in from the 'wiprobe' runtime diff (docs/determinism.md). Empty entries are skipped.
// wiprobe (run24): only +0x284 and +0x288 change tick to tick (+1/60 each: TimeSeconds,
// RealTimeSeconds). +0x280 time scale and +0x28C delta (Codex clock notes) plus the
// one-shot override +0x394/+0x398 are restored with them.
constexpr uint32_t kSkelMeshVtable = 0x00D5A388;
constexpr uint32_t kMkHelperVtable = 0x00C6A5B4, kMkHelperSize = 0x4C;  // MK object +0x1D8 helper (0x0089F2CE)
constexpr uint32_t kMkHelperDeadVtable = 0x00C6A5A8;  // written by its deleting destructor 0x004A5B70 before the free
struct SkelArray {
    const char* name;
    uint32_t offset, elem;
};
const SkelArray kSkelArrays[] = {{"skel_atoms", 0x2C8, 40}, {"skel_m0", 0x2F4, 64}, {"skel_m1", 0x300, 64}, {"skel_m2", 0x30C, 64}};
// The same arrays of the other MK skeletal objects' components (gPropSkel): own names so gPropSkelLoad can gate them.
const SkelArray kPropSkelArrays[] = {{"skelp_atoms", 0x2C8, 40}, {"skelp_m0", 0x2F4, 64}, {"skelp_m1", 0x300, 64}, {"skelp_m2", 0x30C, 64}};
const ClockField kWorldInfoClock[] = {{"wi_time", 0x280, 0x10}, {"wi_override", 0x394, 0x8}};

uintptr_t WorldInfo() {
    // Getter 0x6D5430: **(*(GWorld+0x48)+0x38) = PersistentLevel->Actors[0].
    const uintptr_t world = *reinterpret_cast<const uint32_t*>(0x00E86AB0);
    const uintptr_t level = world ? *reinterpret_cast<const uint32_t*>(world + 0x48) : 0;
    const uintptr_t actors = level ? *reinterpret_cast<const uint32_t*>(level + 0x38) : 0;
    return actors ? *reinterpret_cast<const uint32_t*>(actors) : 0;
}
constexpr uint32_t kCharObjSize = 0xC580;  // MKFGCharacterObj allocation size
constexpr uint32_t kPadObjSize = 0x60C;    // gamepad object (hardware pad array stride at 0x00E221A8)
uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

// The save's "skel" phase (fx/MK actors, helpers, bone arrays, components; SaveAB below) only reads: there a guarded read
// per page answers instead of VirtualQuery, which costs a few microseconds per call in this process (SAVE SKEL
// 2026-11-04, Courtyard: 773 calls per save, the phase 2.4 ms). LoadAB still checks every span with Writable (one region,
// committed, read-write) before it writes. 'fastprobe 0' brings the VirtualQuery checks back.
bool gProbeFast = false;
bool ProbeRead(uintptr_t p, size_t n) {
    if (p < 0x10000 || n == 0 || p + n < p) return false;
    uint32_t v;
    for (uintptr_t a = p & ~uintptr_t(0xFFF); a < p + n; a += 0x1000)
        if (!CrashLog::SafeRead32(a < p ? p : a, &v)) return false;
    return true;
}
bool Writable(uintptr_t p, size_t n) { return gProbeFast ? ProbeRead(p, n) : MemQuery::Writable(p, n); }
// A read-only decision outside the save's guarded-read window (LoadAB's checks before anything is written): guarded
// reads when the crash handler is installed, else the VirtualQuery test as before. Whatever is then written is checked
// with Writable (VirtualQuery) again where it is written (rc25 save/load audit).
bool ReadableForCheck(uintptr_t p, size_t n) { return CrashLog::SafeReadAvailable() ? ProbeRead(p, n) : MemQuery::Writable(p, n); }

// GMalloc block lookup (0x00405A10) plus an allocated test. For medium-heap blocks (0x800 < size <=
// 0x40000, MK heap class 0x00C66A3C) the lookup reads the chunk size without checking that the chunk is in
// use; a freed chunk's header at p-8 is {prev, size|flags} with flag bit 0 clear (heap Free 0x0042B1D0
// merges neighbours whose bit 0 is clear). Restoring content into a freed chunk overwrote the free tree.
// A UObject still registered at its GObjObjects slot (garbage-collected objects are removed).
bool LiveUObject(uint32_t o) {
    if (!Writable(o, 0x40)) return false;
    const uint32_t idx = Read32(o + 4), objs = Read32(0x00E7C0EC), cnt = Read32(0x00E7C0F0);
    return objs && idx < cnt && Read32(objs + 4 * idx) == o;
}
bool LiveBlockInfo(const void* p, uint32_t* info) {
    if (!CrashLog::SafeBlockInfo(p, info)) return false;
    if (info[1] > 0x800 && info[1] <= 0x40000) {
        const uint32_t hdr = *(reinterpret_cast<const uint32_t*>(p) - 1);
        if (!(hdr & 1)) return false;
    }
    return true;
}

// Dword offsets inside [base, base+size) whose value currently looks like a heap
// pointer. These are excluded from restore: the referenced object's lifetime is
// not ours, and restoring a stale pointer is how snapshot schemes crash.
// Heap addresses in this process overlap ordinary float bit patterns
// (1.0f = 0x3F800000, 125.3f = 0x42FA90B0), so the test is asymmetric:
//  - below 0x30000000 a float would be < 5e-10, implausible as gameplay data -> pointer;
//  - above, only a target whose first dword is a vtable inside MKKE's image counts.
// Region cache for one MarkKeep scan only (a span of <= 4 KB, tens of microseconds): a save-wide cache
// went stale against other threads' commits/decommits and crashed (2026-10-15); within one scan the window
// is as small as the uncached VirtualQuery-then-read it replaces. Reset at the start of every scan.
struct ScanRegion {
    uint32_t lo, hi;
    bool ok;
};
ScanRegion gScan[64];
int gScanN = 0;
// Committed regions seen in recent saves (positive answers only), as disjoint address ranges sorted by start. rc25: 1024
// ranges with a binary search instead of 64 searched in turn; the set of addresses answered "committed" is the union of
// the regions VirtualQuery answered since the last reset, as before (overlapping or touching ranges are merged, which
// covers the same addresses), with the same reset (every 120 saves, SaveAB) and the same "stop adding when full". The 64
// filled within a few saves on prop-heavy stages, and every later miss cost a VirtualQuery (263-291 a save in SAVE SKEL).
struct CommittedRange {
    uint32_t lo, hi;
};
constexpr int kCommittedMax = 1024;
CommittedRange gCommitted[kCommittedMax];
int gCommittedN = 0;
uint32_t gCommittedHits = 0, gScanHits = 0, gVqCommitted = 0, gVqOther = 0;  // since the last SAVE SKEL line
bool InCommittedCache(uint32_t v) {
    int a = 0, b = gCommittedN;  // first range with lo > v
    while (a < b) {
        const int m = (a + b) / 2;
        if (gCommitted[m].lo <= v) a = m + 1;
        else b = m;
    }
    return a > 0 && v < gCommitted[a - 1].hi;
}
int gCommittedCap = kCommittedMax;  // 'heapcache N' (rc25 A/B; 64 = rc24's capacity, counted in merged ranges)
void AddCommittedRange(uint32_t lo, uint32_t hi) {
    if (gCommittedN >= gCommittedCap) return;  // full: nothing is added until the next reset (as the 64-entry cache)
    int a = 0, b = gCommittedN;  // first range with hi >= lo (ranges are disjoint, so their ends are sorted too)
    while (a < b) {
        const int m = (a + b) / 2;
        if (gCommitted[m].hi < lo) a = m + 1;
        else b = m;
    }
    int e = a;  // ranges [a, e) overlap or touch [lo, hi)
    while (e < gCommittedN && gCommitted[e].lo <= hi) {
        lo = std::min(lo, gCommitted[e].lo);
        hi = std::max(hi, gCommitted[e].hi);
        ++e;
    }
    if (e == a) {  // a new range
        if (gCommittedN >= gCommittedCap) return;  // full: not added (as the 64-entry cache)
        memmove(&gCommitted[a + 1], &gCommitted[a], (gCommittedN - a) * sizeof gCommitted[0]);
        gCommitted[a] = {lo, hi};
        ++gCommittedN;
        return;
    }
    gCommitted[a] = {lo, hi};
    memmove(&gCommitted[a + 1], &gCommitted[e], (gCommittedN - e) * sizeof gCommitted[0]);
    gCommittedN -= e - a - 1;
}

bool CommittedHeap(uint32_t v) {
    // MKKE is large-address-aware: heap blocks above 2 GB are common (soak28: a fighter's
    // attachment TArray at 0x80EB69F0 was taken for data and restored by value).
    if (v < 0x01000000 || v >= 0xFFFE0000 || (v & 3)) return false;
    for (int i = 0; i < gScanN; ++i)
        if (v >= gScan[i].lo && v < gScan[i].hi) {
            ++gScanHits;
            return gScan[i].ok;
        }
    // Committed regions seen in recent saves (positive answers only: memory gets committed all the time, so a
    // cached 'not committed' would go stale; a stale 'committed' only keeps a field live, and the one read of a
    // target is guarded by SafeRead32). Reset every 120 saves (ResetCommittedCache).
    if (InCommittedCache(v)) {
        ++gCommittedHits;
        return true;
    }
    MEMORY_BASIC_INFORMATION mbi;
    ++MemQuery::gVqCalls;  // counted with MemQuery's (SAVE SKEL)
    if (!VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi)) {
        ++gVqOther;
        return false;
    }
    const bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
    ++(ok ? gVqCommitted : gVqOther);
    if (ok && Snapshot::gPositiveCache) {
        const uint32_t lo = (uint32_t)reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uint32_t hi = lo + (uint32_t)mbi.RegionSize;
        if (hi > lo) AddCommittedRange(lo, hi);
    }
    if (gScanN < 64) {
        const uint32_t lo = (uint32_t)reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uint32_t hi = lo + (uint32_t)mbi.RegionSize;
        if (hi > lo) gScan[gScanN++] = {lo, hi, ok};
    }
    return ok;
}

// Section map of the other loaded modules (PhysXCore, PhysXLoader, ...): their objects live in GMalloc memory
// above 0x30000000 too, and a pointer to one (e.g. an NxActor) restored by value can name a released object
// whose vtable is the abstract base -> R6025 pure virtual call on a PhysX worker (synctest86/87).
struct ModSection {
    uint32_t lo, hi;
    uint16_t mod;
    bool exec;
};
std::vector<ModSection> gModSections;
uint32_t gForeignKeeps = 0;
// ForeignVtable's verdict per address: address | 1 (known) | 2 (a vtable). Scans run on several threads.
std::atomic<uint32_t> gForeignVerdict[1024];
void BuildModSections() {
    std::vector<ModSection> was;
    was.swap(gModSections);
    gModSections.reserve(was.size());
    // The verdicts hold while the sections are the same ones (this runs every 600 saves for modules that load late).
    struct Verdicts {
        const std::vector<ModSection>& was;
        ~Verdicts() {
            auto same = [](const ModSection& a, const ModSection& b) { return a.lo == b.lo && a.hi == b.hi && a.mod == b.mod && a.exec == b.exec; };
            if (was.size() == gModSections.size() && std::equal(was.begin(), was.end(), gModSections.begin(), same)) return;
            for (std::atomic<uint32_t>& e : gForeignVerdict) e.store(0, std::memory_order_relaxed);
        }
    } verdicts{was};
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    const HMODULE exe = GetModuleHandleA(nullptr);
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; ++i) {
        if (mods[i] == exe) continue;  // MKKE: fixed ranges below
        auto* base = reinterpret_cast<const uint8_t*>(mods[i]);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
            const DWORD ch = sec[s].Characteristics;
            const bool exec = ch & IMAGE_SCN_MEM_EXECUTE, write = ch & IMAGE_SCN_MEM_WRITE;
            if (write) continue;  // vtables are in read-only data; code is never writable
            const uint32_t lo = (uint32_t)(uintptr_t)base + sec[s].VirtualAddress;
            gModSections.push_back({lo, lo + sec[s].Misc.VirtualSize, (uint16_t)i, exec});
        }
    }
    std::sort(gModSections.begin(), gModSections.end(), [](const ModSection& a, const ModSection& b) { return a.lo < b.lo; });
}
// Binary search (sorted by lo, non-overlapping): this runs for most heap-range dwords of every scanned span.
const ModSection* FindSection(uint32_t a) {
    auto it = std::upper_bound(gModSections.begin(), gModSections.end(), a,
                               [](uint32_t v, const ModSection& s) { return v < s.lo; });
    if (it == gModSections.begin()) return nullptr;
    --it;
    return a < it->hi ? &*it : nullptr;
}

// Classify over a byte image of the span (saved copy or live memory).
// Marks: pointers (strict rule above) and UE3 TArray headers {T* data; int num; int max}
// (all three dwords: restoring num/max against a different allocation would overflow it).
// Targets on pages outside the working set are not read. A trimmed page (the game minimized, Windows' working-set
// trimming) can take a disk read to come back: on the other player's PC (2026-11-08 22:02-22:19, working set 660-750 MB
// of 890-980 MB private) four 0.8 s freezes were each one SafeRead32 below, on the game thread inside the live save,
// while that PC's game opening a file at a round end took as long (its disk). One QueryWorkingSetEx per scan answers for
// every candidate; one on a page that is not resident is kept live, as a pointer would be (a field kept live is never a
// stale pointer written back; a float taken for one is not rolled back in that save). 'coldcheck 0' reads them all again
// (Snapshot::gColdCheck).
// Those pages are touched by a background thread instead (WarmPages): a trimmed page's disk read then blocks it, not the
// game thread, and a committed page nothing had touched yet (about 40 targets a save in b34, mostly floats landing in
// untouched GMalloc memory) becomes resident as the game thread's own read made it before, so the next saves read and
// classify them as before.
uint32_t gColdKeeps = 0, gColdScans = 0, gWarmQueued = 0;  // since the last COLDKEEP line
struct PageWarmer {
    CRITICAL_SECTION lock;
    HANDLE wake = nullptr, thread = nullptr;
    volatile LONG stop = 0;
    std::vector<uint32_t> queue;
    static DWORD WINAPI Run(void* p) {
        PageWarmer* w = static_cast<PageWarmer*>(p);
        std::vector<uint32_t> batch;
        while (WaitForSingleObject(w->wake, INFINITE) == WAIT_OBJECT_0 && !w->stop) {
            EnterCriticalSection(&w->lock);
            batch.swap(w->queue);
            LeaveCriticalSection(&w->lock);
            for (uint32_t a : batch) {
                if (w->stop) break;
                uint32_t v;
                CrashLog::SafeRead32(a, &v);  // guarded: the page can be decommitted meanwhile
            }
            batch.clear();
        }
        return 0;
    }
    PageWarmer() {
        InitializeCriticalSection(&lock);
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake) thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        if (thread) SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
};
PageWarmer* gWarmer = nullptr;
void WarmPages(const std::vector<uint32_t>& pages) {
    static PageWarmer* w = (gWarmer = new PageWarmer());  // thread-safe one-time start; never freed (see StopPageWarmer)
    if (!w->thread || w->stop || pages.empty()) return;
    EnterCriticalSection(&w->lock);
    if (w->queue.size() < 4096) w->queue.insert(w->queue.end(), pages.begin(), pages.end());
    LeaveCriticalSection(&w->lock);
    SetEvent(w->wake);
}
struct WsExInfo {
    void* va;
    ULONG_PTR attr;  // PSAPI_WORKING_SET_EX_BLOCK: bit 0 Valid (in the working set)
};
// A vtable of another module: 4-aligned, in a read-only non-code section, slot 0 in code of the same module.
// The verdict per address is kept (gForeignVerdict), so the module's page is read once and not in every save. On the
// other player's PC (two-PC match of 2026-11-11, 18:36:48) one frame's live save took 812.8 ms in the SafeRead32 below:
// the DLL's page had left the working set and came back from that PC's disk, the 0.8 s of the heap targets of
// 2026-11-08 (above). An address without a verdict whose page is not resident answers "a vtable" unread (kept live, as
// MarkKeepOrigins does for a cold target), and the warming thread fetches the page for the next saves.
bool ForeignVtable(uint32_t first) {
    if (first & 3) return false;
    std::atomic<uint32_t>& kept = gForeignVerdict[(first * 0x9E3779B1u) >> 22];
    const uint32_t e = kept.load(std::memory_order_relaxed);
    if ((e & 1) && (e & ~3u) == first) return (e & 2) != 0;
    const ModSection* d = FindSection(first);
    if (!d || d->exec) return false;
    if (Snapshot::gColdCheck) {
        WsExInfo ws{reinterpret_cast<void*>(static_cast<uintptr_t>(first)), 0};
        if (K32QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof ws) && !(ws.attr & 1)) {
            thread_local std::vector<uint32_t> page(1);
            page[0] = first & ~0xFFFu;
            ++gColdKeeps;
            ++gWarmQueued;
            WarmPages(page);
            return true;
        }
    }
    uint32_t slot0 = 0;
    if (!CrashLog::SafeRead32(first, &slot0)) return false;
    const ModSection* c = FindSection(slot0);
    const bool vtable = c && c->exec && c->mod == d->mod;
    kept.store(first | 1u | (vtable ? 2u : 0u), std::memory_order_relaxed);
    return vtable;
}
// The scan over a chosen set of origin dwords (rc25, LoadAB's diff-first merge): 'origins' lists dword offsets (ascending,
// each + 4 <= size), or null for every dword of the span (MarkKeep). A dword o can only be marked by the origins o - 8,
// o - 4 and o (a TArray triple marks its start and the next two dwords; every other rule marks the origin itself), and
// each origin's rules read only the dwords o .. o + 8 of 'bytes' and its target's memory, so scanning a subset that
// contains o - 8, o - 4 and o gives keep[o / 4] exactly as the whole scan does.
void MarkKeepOrigins(const uint8_t* bytes, uint32_t size, const uint32_t* origins, size_t nOrigins, std::vector<uint8_t>& keep);
void MarkKeep(const uint8_t* bytes, uint32_t size, std::vector<uint8_t>& keep) { MarkKeepOrigins(bytes, size, nullptr, 0, keep); }
void MarkKeepOrigins(const uint8_t* bytes, uint32_t size, const uint32_t* origins, size_t nOrigins, std::vector<uint8_t>& keep) {
    constexpr uint32_t kImageLo = 0x00401000, kImageHi = 0x010AE000;
    constexpr uint32_t kTextHi = 0x00C54FD4, kRdataLo = 0x00C55000, kRdataHi = 0x00DE064B;  // MKKE D1C71E08 sections
    auto at = [&](uint32_t o) { uint32_t v; memcpy(&v, bytes + o, 4); return v; };
    // A target above 0x30000000 counts when it starts with a real vtable (see below); classified after the residency
    // query when gColdCheck.
    auto classify = [&](uint32_t o, uint32_t v) {
        // Guarded: v was committed when VirtualQuery ran, but a worker thread can decommit it before this read
        // (synctest41: read of 0x3F800000 = 1.0f in the fightercomp scan).
        uint32_t first = 0;
        if (!CrashLog::SafeRead32(v, &first)) return;
        if (Snapshot::gStrictVtable) {
            if (!(first & 3) && first >= kRdataLo && first < kRdataHi) {
                const uint32_t slot0 = Read32(first);
                if (slot0 >= kImageLo && slot0 < kTextHi) keep[o / 4] = 1;
            } else if (Snapshot::gForeignVtables && first >= 0x01000000 && ForeignVtable(first)) {
                keep[o / 4] = 1;
                ++gForeignKeeps;
            }
        } else if (first >= kImageLo && first < kImageHi) {
            keep[o / 4] = 1;
        }
    };
    thread_local std::vector<uint32_t> pending;  // (offset, target) pairs
    thread_local std::vector<WsExInfo> ws;
    pending.clear();
    gScanN = 0;  // fresh region cache for this scan (a save-wide cache was followed by a task-wait hang, synctest57)
    const size_t nScan = origins ? nOrigins : size / 4;
    for (size_t idx = 0; idx < nScan; ++idx) {
        const uint32_t o = origins ? origins[idx] : static_cast<uint32_t>(idx) * 4;
        if (o + 4 > size || (o & 3)) continue;  // (never for the whole scan: o = 4 * idx <= size - 4)
        const uint32_t v = at(o);
        if (!CommittedHeap(v)) continue;
        if (o + 12 <= size) {
            const int32_t num = (int32_t)at(o + 4), max = (int32_t)at(o + 8);
            // Above 0x30000000 the 'data pointer' may be a float followed by two small ints (fighter Z, pitch 0,
            // yaw 0x4000 looked like {data, 0, 16384} and kept Location+Rotation live, LEARNFIELDS synctest63):
            // GMalloc array blocks are 8-aligned.
            if (num >= 0 && max > 0 && num <= max && max <= 0x100000 && (v < 0x30000000 || !(v & 7) || !Snapshot::gStrictVtable)) {
                keep[o / 4] = keep[o / 4 + 1] = keep[o / 4 + 2] = 1;
                continue;
            }
        }
        if (v < 0x30000000) {
            keep[o / 4] = 1;
            continue;
        }
        // Above 0x30000000 a heap address is also an ordinary float (0.7 .. 2e11), so only a target that starts
        // with a real vtable counts: a 4-aligned .rdata address whose first slot is code. Image-range first
        // dwords that are code/.data addresses or unaligned kept floats live (VTKEEP 07:30: anim node +0x5C =
        // 26.1 -> "vt" 0x0048D12F, actor +0x1CC = 35.0 -> 0x00D7E3E7), so they were never restored.
        if (!Snapshot::gColdCheck) {
            classify(o, v);
            continue;
        }
        pending.push_back(o);
        pending.push_back(v);
    }
    if (pending.empty()) return;
    const size_t n = pending.size() / 2;
    ws.resize(n);
    for (size_t i = 0; i < n; ++i) ws[i] = {reinterpret_cast<void*>(static_cast<uintptr_t>(pending[2 * i + 1])), 0};
    ++gColdScans;
    const bool known = K32QueryWorkingSetEx(GetCurrentProcess(), ws.data(), static_cast<DWORD>(n * sizeof(WsExInfo))) != 0;
    thread_local std::vector<uint32_t> cold;
    cold.clear();
    for (size_t i = 0; i < n; ++i) {
        const uint32_t o = pending[2 * i], v = pending[2 * i + 1];
        if (!known || (ws[i].attr & 1)) {
            classify(o, v);
        } else {
            keep[o / 4] = 1;
            ++gColdKeeps;
            const uint32_t page = v & ~0xFFFu;
            if (cold.empty() || cold.back() != page) cold.push_back(page);
        }
    }
    if (!cold.empty()) {
        gWarmQueued += static_cast<uint32_t>(cold.size());
        WarmPages(cold);
    }
}

// Diagnostic: dwords the vtable rule (>= 0x30000000, target starts with an image address) keeps live whose
// target is not a live UObject: candidates for floats taken for pointers (heap addresses overlap floats).
int gVtKeepReports = 0;
bool gVtKeepDiag = false;  // done (07:30, led to strictvt); its reads are unguarded
void VtableKeepReport(const char* name, uintptr_t base, uint32_t size) {
    if (!gVtKeepDiag || gVtKeepReports >= 24) return;
    constexpr uint32_t kImageLo = 0x00401000, kImageHi = 0x010AE000;
    char line[512];
    int n = 0, shown = 0, total = 0, uobj = 0;
    gScanN = 0;
    for (uint32_t o = 0; o + 4 <= size; o += 4) {
        const uint32_t v = Read32(base + o);
        if (v < 0x30000000 || !CommittedHeap(v)) continue;
        if (o + 12 <= size) {
            const int32_t num = (int32_t)Read32(base + o + 4), max = (int32_t)Read32(base + o + 8);
            if (num >= 0 && max > 0 && num <= max && max <= 0x100000) continue;  // TArray rule
        }
        const uint32_t first = Read32(v);
        if (first < kImageLo || first >= kImageHi) continue;
        const uint32_t idx = Read32(v + 4), objs = Read32(0x00E7C0EC), cnt = Read32(0x00E7C0F0);
        if (objs && idx < cnt && Read32(objs + 4 * idx) == v) {
            ++uobj;
            continue;
        }
        ++total;
        float f;
        memcpy(&f, &v, 4);
        if (shown++ < 8) n = AppendF(line, sizeof line, n, " +%X=%08X(%g vt %08X)", o, v, f, first);
    }
    line[n] = 0;
    if (total) {
        ++gVtKeepReports;
        Logger::Info("VTKEEP %s: %d non-UObject vtable-rule keeps (%d UObject):%s", name, total, uobj, line);
    }
}

// Learned field kinds for pointer-merged spans: per address, how often its nonzero value looked like a pointer and how
// often like data, over the saves of the fight. A float that coincides with an object address once in a while (a
// fighter coordinate, synctest59) is data in most saves; real pointer fields never look like data.
std::unordered_map<uint32_t, std::pair<uint16_t, uint16_t>> gFieldKind;
int gLearnedRestores = 0;
std::map<std::string, std::pair<int, uint32_t>> gLearnedWhere;  // span+offset -> (count, last saved value)  // address -> (pointer-like, data-like)
void LearnFields(uintptr_t base, uint32_t size, const std::vector<uint8_t>& keep) {
    if (gFieldKind.size() > 400000) gFieldKind.clear();  // objects of earlier fights
    for (uint32_t o = 0; o + 4 <= size; o += 4) {
        const uint32_t v = Read32(base + o);
        if (!v) continue;
        auto& k = gFieldKind[(uint32_t)(base + o)];
        if (keep[o / 4]) {
            if (k.first < 60000) ++k.first;
        } else if (k.second < 60000) {
            ++k.second;
        }
    }
}
bool LearnedData(uint32_t addr) {
    auto it = gFieldKind.find(addr);
    return it != gFieldKind.end() && it->second.second >= 3 && it->second.second > it->second.first;
}

// Mode 2 only reports: a map operation per nonzero dword of every pointer-merged span at every save (replays included)
// and a string-keyed record per learned field at loads. Players run it only in mode 1 ('learnfields 1').
bool LearnFieldsOn() { return Snapshot::gLearnFields == 1 || (Snapshot::gLearnFields == 2 && Lab::On()); }

std::vector<uint32_t> PointerLikeOffsets(uintptr_t base, uint32_t size) {
    std::vector<uint8_t> keep(size / 4, 0);
    MarkKeep(reinterpret_cast<const uint8_t*>(base), size, keep);
    if (LearnFieldsOn()) LearnFields(base, size, keep);
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < keep.size(); ++i)
        if (keep[i]) out.push_back(i * 4);
    return out;
}

uintptr_t CharObj(int player, uint32_t* id) {
    const uintptr_t info = MK9Game::Get().playerInfoBase + player * MK9Game::kPlayerInfoStride;
    const uintptr_t obj = Read32(info + 0x1C);
    *id = Read32(info + 0x20);
    if (obj == 0 || obj == 0xDEADBEEF || *id == 0xFFFFFFFF || Read32(obj + 4) != *id) return 0;
    return obj;
}
}  // namespace

namespace Snapshot {
void SetCommittedCap(int n) {
    gCommittedCap = n < 0 ? 0 : n > kCommittedMax ? kCommittedMax : n;
    gCommittedN = 0;  // starts over (as the 120-save reset)
    Logger::Info("HEAPCACHE %d: the pointer scan keeps up to %d committed ranges between resets (rc24: 64)", gCommittedCap, gCommittedCap);
}
bool gFastProbe = true;
const std::vector<uint8_t>& ReflectDataMask(uint32_t cls);  // UE3 reflection: data-only dwords of a class (below)
void SaveBindState(uint32_t comp, std::vector<uint32_t>& r);        // fighter SkelControl binding state (below)
bool RestoreBindState(uint32_t liveComp, const std::vector<uint32_t>& r);

bool gSkelArrays = true;
bool gScriptScratch = true;
bool gTierEOn = true;
bool gScriptData = true;
bool gCompSpan = true;
bool gTierENodes = true;
bool gTierETickNodes = true;  // also every node in the fighter components' tick arrays
bool gStrictVtable = true;
bool gForeignVtables = true;
bool gColdCheck = true;
int gTrimWsMs = 0;
void StopPageWarmer() {  // MK9R_Unload: the thread must leave this module's code before it is unloaded
    PageWarmer* w = gWarmer;
    if (!w || !w->thread) return;
    InterlockedExchange(&w->stop, 1);
    SetEvent(w->wake);
    WaitForSingleObject(w->thread, 3000);
}
bool gFloatFields = true;
bool gPositiveCache = true;  // persistent cache of committed regions for the pointer scan ('poscache 0|1')
int gLearnFields = 2;  // learned pointer/data kind: 0 off, 1 restore learned-data fields, 2 report only ('learnfields N'). synctest62 (1): 2.6%/2.0% vs 0.7%/1.3%  // actor Location is always restored by value ('floatfields 0|1')
bool gNameMaps = true;  // fighters' name-keyed lookup cache TMap at obj+0x138: blocks + header consistency ('namemaps 0|1')
uint32_t gCompListRebased = 0;
uint32_t gPendingSkips = 0;  // spans not captured: their block is already freed (in the quarantine)
bool gCompListRebase = false;  // restore the saved count/entries into a reallocated live list ('complistrebase 0|1'); in-fight A/B: no effect (ON 37 vs OFF 32 errors / 433 loads)  // loads where the queued list was restored into a different (live) block
bool gCompList = true;  // fighter component queued anim-object list (+0x3A8): contents + count ('complist 0|1')
bool gBoneRotation = true;  // restore the saved rotation of the triple-buffered bone matrices ('bonerot 0|1')
// Pose of the other MK skeletal objects' components (props attached to a fighter): saved always, applied at loads
// while gPropSkelLoad ('propskel 0|1', abtest 'propskel').
bool gPropSkel = true, gPropSkelLoad = true;
uint32_t gPropSkelSaved = 0, gPropSkelRestored = 0, gPropSkelSkipped = 0;
int gBoneRotations = 0;
bool gSkelBind = false;  // restore fighter SkelControls' bound object (+0xC0) at load ('abtest skelbind')
bool gSkelBound = false;     // capture only the bound elements ('skelbound'; superseded by compweights)
bool gCompWeights = false;   // capture the component's bound-object (+0x384) and weight (+0x39C) arrays
bool gCompWeightsLoad = true;  // restore them at load ('abtest compweights')
bool gSkelBoundLoad = true;  // restore them at load ('abtest skelbound')
constexpr uint32_t kSkelBoundVtable = 0x00D5A5E0;
uint32_t gSkelBindRestores = 0, gSkelBindKept = 0;
bool gPawnComps = false;  // synctest23: 6.1% desync (vs 4%) and a 0x42AF1D crash
bool gTierECamera = false;  // synctest21: restoring them doubled the desync rate (8.5% vs ~4%)

std::vector<Span> CandidateSpans() {
    gScanN = 0;  // fresh region cache for this capture
    std::vector<Span> spans;
    const uintptr_t gameInfo = Read32(MK9Game::Get().gameInfoPtr);
    spans.push_back({"rng", 0x00E7C054, 4, 0, {}});
    spans.push_back({"rngB", 0x00E5352C, 4, 0, {}});  // GSRandSeed (FX/UI, some script natives)
    // GameInfo head incl. GameFrame (+0x70: input rings and move timers are stamped
    // with it); HostFrame (+0x6C) keeps running like a wall clock.
    spans.push_back({"gameinfo", gameInfo, 0x20C, 0, {0x6C}});
    for (int i = 0; i < 2; ++i) {
        const uintptr_t info = MK9Game::Get().playerInfoBase + i * MK9Game::kPlayerInfoStride;
        Span s{i ? "pinfo1" : "pinfo0", info, MK9Game::kPlayerInfoStride, 0, PointerLikeOffsets(info, MK9Game::kPlayerInfoStride)};
        spans.push_back(s);
        const uintptr_t pad = Read32(info + 0xB40);
        if (pad) spans.push_back({i ? "pad1" : "pad0", pad, kPadObjSize, 0, PointerLikeOffsets(pad, kPadObjSize)});
        uint32_t id = 0;
        const uintptr_t obj = CharObj(i, &id);
        if (obj) {
            spans.push_back({i ? "char1" : "char0", obj, kCharObjSize, id, PointerLikeOffsets(obj, kCharObjSize)});
            // UE3 actor (location at +0xDC). Size is provisional until the class size is known.
            const uintptr_t actor = Read32(obj + 0x08);
            if (actor && Writable(actor, kActorSpan))
                spans.push_back({i ? "actor1" : "actor0", actor, kActorSpan, 0, PointerLikeOffsets(actor, kActorSpan)});
        }
    }
    return spans;
}

void Save(State* out, uint32_t gameFrame) {
    out->gameFrame = gameFrame;
    out->spans = CandidateSpans();
    out->bytes.resize(out->spans.size());
    for (size_t i = 0; i < out->spans.size(); ++i) {
        const Span& s = out->spans[i];
        out->bytes[i].resize(s.size);
        memcpy(out->bytes[i].data(), reinterpret_cast<const void*>(s.base), s.size);
    }
}

int Load(const State& st) {
    gScanN = 0;  // fresh region cache for this load
    int restored = 0;
    MKHeap::Region mr[8];
    const int nmr = MKHeap::MasterRanges(mr, 8);
    for (size_t i = 0; i < st.spans.size(); ++i) {
        const Span& s = st.spans[i];
        if (s.identity && Read32(s.base + 4) != s.identity) {
            Logger::Info("SNAPSHOT skip %s: identity changed", s.name);
            continue;
        }
        if (!Writable(s.base, s.size)) {
            Logger::Info("SNAPSHOT skip %s: not writable", s.name);
            continue;
        }
        // Keep-set = pointer-like at save time (recorded) U pointer-like in live memory now.
        std::vector<uint8_t> keep(s.size / 4, 0);
        for (uint32_t o : s.keepOffsets) keep[o / 4] = 1;
        if (!s.keepOffsets.empty() || s.size > 4)  // rng (4 bytes) has no pointers
            MarkKeep(reinterpret_cast<const uint8_t*>(s.base), s.size, keep);
        std::vector<uint8_t> merged = st.bytes[i];
        for (uint32_t k = 0; k < keep.size(); ++k)
            if (keep[k]) memcpy(merged.data() + k * 4, reinterpret_cast<const void*>(s.base + k * 4), 4);
        memcpy(reinterpret_cast<void*>(s.base), merged.data(), s.size);
        ++restored;
    }
    return restored;
}

// ---- Tier A + B (docs/state_ownership.md §5) ----

namespace {
// UE3 actor fields written by the renderer, not the simulation (runtime-observed in
// identity rollbacks, run21): +0xAC (float ~ world seconds; LastRenderTime-like) and
// bit 0x40 of the flags dword at +0x80. Never rolled back, never compared.
constexpr uint32_t kActorRenderTime = 0xAC, kActorFlags = 0x80, kActorRenderBit = 0x40;
void MaskActorRenderFields(uint8_t* b) {
    memset(b + kActorRenderTime, 0, 4);
    uint32_t f;
    memcpy(&f, b + kActorFlags, 4);
    f &= ~kActorRenderBit;
    memcpy(b + kActorFlags, &f, 4);
}
void KeepActorRenderFields(uint8_t* merged, const uint8_t* live) {
    memcpy(merged + kActorRenderTime, live + kActorRenderTime, 4);
    uint32_t m, l;
    memcpy(&m, merged + kActorFlags, 4);
    memcpy(&l, live + kActorFlags, 4);
    m = (m & ~kActorRenderBit) | (l & kActorRenderBit);
    memcpy(merged + kActorFlags, &m, 4);
}
}  // namespace

namespace {
struct Raw {
    const char* name;
    uintptr_t base;
    uint32_t size;
};

std::vector<Raw> TierA() {
    std::vector<Raw> r;
    // GameInfo (~0x82E0) incl. PlayerInfo[4]; HostFrame (+0x6C) is kept live by LoadAB.
    r.push_back({"gameinfo_all", 0x00E89B70, 0x82E0});
    r.push_back({"hwpads", 0x00E221A8, 6 * 0x60C});
    for (int i = 0; i < 2; ++i) {  // AI/remote pads live outside the hardware array (Practice P2: 0xE27C5C)
        const uintptr_t pad = Read32(MK9Game::Get().playerInfoBase + i * MK9Game::kPlayerInfoStride + 0xB40);
        // The whole 0x60C pad object (the hardware array's stride): the press ring's second index byte is at
        // +0x5F8, just past the release ring. With 0x5F8 bytes it stayed live, and the input-history queries
        // 0x00425D80/0x00426EC0 (compare +0x54 with +0x5F8) answered differently in a replay. That was the first
        // divergence on 6 of 10 'r' frames: script natives 0x008CB8E0/0x008CB0D0/0x008CBD10 -> 1 live, 0 replay
        // (gated_1/2, 2026-10-18).
        if (pad && (pad < 0x00E221A8 || pad >= 0x00E221A8 + 6 * 0x60C)) r.push_back({"aipad", pad, kPadObjSize});
    }
    r.push_back({"rngA", 0x00E7C054, 4});
    r.push_back({"rngB", 0x00E5352C, 4});
    // Script RandomInt's private stream (ScriptRng.h): seed + fight key, in this DLL's own memory.
    r.push_back({"scriptrng", reinterpret_cast<uintptr_t>(ScriptRng::StatePtr()), sizeof(ScriptRng::State)});
    r.push_back({"mkobj_globals", 0x00DE820C, 0x2C});      // id counter + node lists 0xDE8210/0xDE8224 {?, head, tail, ?, pending} (0x14 each)
    r.push_back({"mkobj_unlink", 0x00E393D8, 0x130});      // pending-sweep array [64] + count, node free list/base, flags 0xE394EC/F0, list 0xE394F4 (swept via 0x0042DBE0)
    r.push_back({"mkobj_lists2", 0x00E39CD8, 0x28});       // two more node lists 0xE39CD8/0xE39CEC (found by listscan, 2026-10-14)
    r.push_back({"mkanim_list", 0x00E3D7E0, 0x28});        // MK anim node list 0xE3D7E0 (0x4D6160/0x4D8221) + counters 0xE3D7F4/F8 + flags 0xE3D804
    r.push_back({"fight_camera", 0x00DF0BA0, 0xA0});       // fight camera / bounds floats 0xDF0BA4..0xDF0C3F, written every fight frame by 0x8AF4xx..0x8B59xx (next to FightUpdatePlayers)
    r.push_back({"mkobj_groups", 0x00E39DF8, 0x1CC});      // 21 tick-group node lists 0xE39E00 (+0x14 each; obj+0x44 = group, 0x42DDC0/0x42DFE0) + refs 0xE39FA4/0xE39FB0 {vt, obj, id}
    // Script event listeners: node list (same constructor 0x42D660 as the tick list), registered by native 0x8B6EA0 ->
    // 0x407CD0 -> 0x407460, walked by the event broadcast 0x407540 (spawns a handler VM per listener of the event). The
    // nodes are in the MK heap; with the header unrestored, a load across a listener add/remove left the replaying
    // game with another listener set (two-instance test 2026-10-18: one extra handler VM at session frame 1412).
    r.push_back({"mkevent_listeners", 0x00E0B4F0, 0x14});
    r.push_back({"mkobj_ticklist", 0x00E0B504, 0x14});     // node list {?, head +4 (iterated at 0x433B60), tail, ?, pending +0x10}; soak6 0x42D9B6 sweep on it
    // MK node list header {?, head, tail, ?, pending} of the MKAnimTrackMatcher list. Only its head (0xE3B698) used to be
    // restored: tail/pending stayed live, the list went inconsistent after a load and cross-linked the global object list,
    // then corrupted the object heap free tree (0x0042AF1D, synctest80/96/100/105/109/111; found by LISTHEADERS).
    r.push_back({"mkanimtrack_list", 0x00E3B694, 0x14});
    // MKScript stopwatch tables (0x18 per slot; ScriptClock): us 0xE3B6D8 (9 slots), ms 0xE3B7B0 (7 slots).
    // Exact bounds from code references (tools/immrange.mjs): 0xE3B858.. are other globals (a gate read by
    // EngineTick, CRT statics, heap pointers); restoring a guessed 0x1D0 range corrupted the GMalloc heap.
    r.push_back({"script_clocks", 0x00E3B6D8, 0x180});
    r.push_back({"misc_E3D7E0", 0x00E3D7E0, 4});
    r.push_back({"misc_E3CE34", 0x00E3CE34, 4});
    r.push_back({"script_globals", 0x00E39DF0, 0x1D8});    // 21 list descriptors + script globals
    for (int i = 0; i < MKHeap::kPoolCount; ++i) {
        const uintptr_t p = Read32(MKHeap::kPoolPtrs + 4 * i);
        if (p) r.push_back({"pool", p, 0x50});  // fields up to +0x4C (runtime peek 2026-10-14)
    }
    // All five MK heap headers (master, object heap, sub-heaps [0xED53F8/FC/0x5400]). Their
    // regions are restored as one image inside the master ranges (run46: a header restored
    // without its region desynchronized the in-region free tree -> OOM hang; soak4: a sub-heap
    // region left live -> double free).
    for (uintptr_t hp : {MKHeap::kMasterHeapPtr, MKHeap::kObjectHeapPtr, (uintptr_t)0x00ED53F8, (uintptr_t)0x00ED53FC,
                         (uintptr_t)0x00ED5400})
        // 0xC0, not 0x3C: from +0x3C the header holds per-size bin state (runtime peek: an array of
        // -1/0 entries up to at least +0x9C; same-class headers are 0xC0 apart). Restoring only
        // 0x3C left bins pointing into the rewound region -> free-tree corruption at 0x0042AF1D.
        // Correction (2026-10-17, SPANAUDIT): class 0xC66A3C sub-heaps are GMalloc(0x3C, 8) objects (0x00887601,
        // 0x0088773E) in 0x40-byte slots, so 0xC0 covered two NEIGHBOURING slots (the "-1/0 bin array" and the
        // "0xC0 apart" were other objects). The 0x0042AF1D crash this was meant for was the partial
        // mkanimtrack_list header. Now: 0x3C for that class, and never past the GMalloc block.
        if (const uintptr_t h = Read32(hp)) {
            uint32_t size = Read32(h) == 0x00C66A3C ? 0x3C : 0xC0;
            uint32_t info[2] = {0, 0};
            if (!(h & 7) && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(h), info) && info[1] >= 0x3C && info[1] < size)
                size = info[1];
            r.push_back({"heapobj", h, size});
        }
    return r;
}
}  // namespace

namespace {
// GMalloc block lookup used by Realloc (0x00405A10, cdecl(ptr, out[2]) -> bool; out[1] = block size).
constexpr uint32_t kMaxExternalBytes = 4u << 20;

// External array blocks owned by MK animation nodes (typed, not heuristic: a generic
// {ptr,num,max} scan restored UE3 objects misread as arrays and corrupted them, run37).
// MK objects are enumerated through the global node list [0x00DE8214] (nodes:
// {vt, obj +4, id +8, next +0xC, prev +0x10, desc +0x14, dead +0x18}). For every live
// object whose type chain contains MKAnimNodeBase:
//   +0x08 TArray {data, num, max}  (released by the base dtor via 0x004CC5E0)
//   +0x3C TArray children          (MKAnimNodeBlend*: records walked at 0x004D1A93)
// The data blocks live in GMalloc; their size comes from the guarded block lookup.
std::vector<uint32_t> gVmContexts;  // [MKScriptVM + 0x24] of every live executor, refreshed by each walk
bool IsScriptVmType(uint32_t desc) {
    static std::unordered_map<uint32_t, bool> cache;
    auto it = cache.find(desc);
    if (it != cache.end()) return it->second;
    const char* name = reinterpret_cast<const char*>(*reinterpret_cast<const uint32_t*>(desc));
    const bool v = reinterpret_cast<uintptr_t>(name) >= 0x00401000 && reinterpret_cast<uintptr_t>(name) < 0x010AE000 &&
                   !strcmp(name, "MKScriptVM");
    return cache.emplace(desc, v).first->second;
}

bool IsSkeletalObjType(uint32_t desc) {
    static std::unordered_map<uint32_t, bool> cache;
    auto it = cache.find(desc);
    if (it != cache.end()) return it->second;
    bool v = false;
    for (uint32_t d = desc, guard = 0; d >= 0x00401000 && d < 0x010AE000 && guard < 16; d = *reinterpret_cast<const uint32_t*>(d + 8), ++guard) {
        const char* name = reinterpret_cast<const char*>(*reinterpret_cast<const uint32_t*>(d));
        if (reinterpret_cast<uintptr_t>(name) < 0x00401000 || reinterpret_cast<uintptr_t>(name) >= 0x010AE000) break;
        if (!strcmp(name, "MKSkeletalObj")) v = true;
    }
    return cache.emplace(desc, v).first->second;
}

bool IsAnimNodeType(uint32_t desc, bool* blend) {
    static std::unordered_map<uint32_t, uint8_t> cache;  // 0 no, 1 anim, 2 anim+blend
    auto it = cache.find(desc);
    if (it == cache.end()) {
        uint8_t v = 0;
        for (uint32_t d = desc, guard = 0; d >= 0x00401000 && d < 0x010AE000 && guard < 16;
             d = *reinterpret_cast<const uint32_t*>(d + 8), ++guard) {
            const char* name = reinterpret_cast<const char*>(*reinterpret_cast<const uint32_t*>(d));
            if (reinterpret_cast<uintptr_t>(name) < 0x00401000 || reinterpret_cast<uintptr_t>(name) >= 0x010AE000) break;
            if (!strcmp(name, "MKAnimNodeBase")) v |= 1;
            if (!strncmp(name, "MKAnimNodeBlend", 15)) v |= 2;
        }
        it = cache.emplace(desc, (v & 1) ? v : 0).first;
    }
    *blend = (it->second & 2) != 0;
    return it->second != 0;
}

void FindExternalBlocks(uintptr_t heapBase, uint32_t heapSize, std::vector<std::pair<uintptr_t, uint32_t>>& out) {
    out.clear();
    static std::unordered_set<uintptr_t> seen;
    seen.clear();
    uint32_t total = 0;
    int capped = 0;  // blocks rejected by kMaxExternalBytes
    int rej[5] = {0, 0, 0, 0, 0};  // null, not a triple, in master image, no GMalloc block, size
    auto take = [&](uintptr_t arr) {
        const uint32_t p = Read32(arr);
        const int32_t num = (int32_t)Read32(arr + 4), max = (int32_t)Read32(arr + 8);
        if (!p) { ++rej[0]; return; }
        if (num < 0 || max <= 0 || num > max || max > 0x100000) { ++rej[1]; return; }
        if (p >= heapBase && p < heapBase + heapSize) { ++rej[2]; return; }
        uint32_t info[2] = {0, 0};
        if (!LiveBlockInfo(reinterpret_cast<const void*>(p), info)) { ++rej[3]; return; }
        const uint32_t size = info[1];
        if (size == 0 || size > 0x100000) { ++rej[4]; return; }
        if (total + size > kMaxExternalBytes) {
            ++capped;
            return;
        }
        if (!seen.insert(p).second) return;
        out.emplace_back(p, size);
        total += size;
    };
    int guard = 0, outside = 0, dead = 0, stale = 0, anim = 0;
    gVmContexts.clear();
    const uint32_t head = Read32(0x00DE8214);
    for (uint32_t node = head; node && guard < 20000; node = Read32(node + 0xC), ++guard) {
        if (node < 0x01000000 || (node & 3)) break;
        if (node < heapBase || node >= heapBase + heapSize) ++outside;
        const uint32_t obj = Read32(node + 4), id = Read32(node + 8);
        if (Read32(node + 0x18) || !obj) { ++dead; continue; }
        if (Read32(obj + 4) != id) { ++stale; continue; }
        // Type descriptor via the object's vt[0] leaf "mov eax, desc; ret" (node+0x14 is not it).
        const uint32_t vt = Read32(obj);
        if (vt < 0x00C40000 || vt >= 0x00DA0000) continue;
        const uint8_t* fn = reinterpret_cast<const uint8_t*>(Read32(vt));
        if (reinterpret_cast<uintptr_t>(fn) < 0x00401000 || reinterpret_cast<uintptr_t>(fn) >= 0x00F9C000) continue;
        if (fn[0] != 0xB8 || fn[5] != 0xC3) continue;
        uint32_t desc;
        memcpy(&desc, fn + 1, 4);
        bool blend = false;
        if (IsScriptVmType(desc)) {  // executor: its context (+0x24, runtime-verified) owns script data
            const uint32_t ctx = Read32(obj + 0x24);
            if (ctx && std::find(gVmContexts.begin(), gVmContexts.end(), ctx) == gVmContexts.end()) gVmContexts.push_back(ctx);
            continue;
        }
        if (IsSkeletalObjType(desc)) {
            // Animation channels: TArray at +0x1B0 of 0x18-byte records (lookup 0x0043B070); each
            // record points at the playback objects at +0x10/+0x14 (current time at +0x74, read by
            // the script native 0x008C80F0 via 0x00440FF0). They are GMalloc blocks reallocated per
            // animation, so their per-frame time was not restored (native trace 2026-10-14).
            const uint32_t data = Read32(obj + 0x1B0);
            const int32_t num = (int32_t)Read32(obj + 0x1B4);
            take(obj + 0x1B0);
            for (int32_t r = 0; data && r < num && r < 256; ++r) {
                const uintptr_t rec = data + 0x18 * r;
                // +4: the channel's bone-mask object (0x0043B070 tests [[rec+4]+0x18+4*(bone>>5)]); on the
                // [obj+0x19C]==0 path it is also the playback object whose +0x74 is the position. Not
                // captured, it made the lookup miss right after a load (soak30: half the first-frame
                // divergences were 0x008C80F0 'X -> 0'; static analysis 2026-10-14).
                for (uint32_t off : {0x4u, 0x10u, 0x14u}) {
                    const uint32_t p = Read32(rec + off);
                    if (!p || (p >= heapBase && p < heapBase + heapSize)) continue;
                    uint32_t info[2] = {0, 0};
                    if (!LiveBlockInfo(reinterpret_cast<const void*>(p), info)) continue;
                    const uint32_t size = info[1];
                    if (size == 0 || size > 0x4000 || total + size > kMaxExternalBytes || !seen.insert(p).second) continue;
                    out.emplace_back(p, size);
                    total += size;
                }
            }
            continue;
        }
        if (!IsAnimNodeType(desc, &blend)) continue;
        ++anim;
        take(obj + 0x08);
        if (blend) take(obj + 0x3C);  // children {.., child +4, id +8} (stride 0x3C)
    }
    static int logged = 0;
    if (logged++ < 3 || logged % 600 == 0)
        Logger::Info("XBLOCK walk: head=%08X nodes=%d outsideHeap=%d dead=%d stale=%d animNodes=%d blocks=%u bytes=%u capped=%d "
                     "rejected null=%d triple=%d master=%d noblock=%d size=%d", head, guard, outside, dead, stale, anim,
                     (unsigned)out.size(), total, capped, rej[0], rej[1], rej[2], rej[3], rej[4]);
}}  // namespace

namespace {
// MK node lists ({?, head +4, tail +8, ? +0xC, pending +0x10}) that live outside the image and
// outside the master-heap image, i.e. embedded in UE3/GMalloc objects (runtime 2026-10-14:
// one with 7 nodes at 0x42F6FC08 in some arenas). Found through the node pool: every live node
// (vt 0x00C66B14) names its list at +0x14; free nodes have vt 0x00D4D980.
void ExternalLists(const MKHeap::Region* mr, int nmr, std::vector<uintptr_t>& out) {
    out.clear();
    const uint32_t base = Read32(0x00E394E4);
    // One region query for the whole pool (a per-node VirtualQuery made every save ~10 ms).
    MEMORY_BASIC_INFORMATION mbi;
    if (!base || !VirtualQuery(reinterpret_cast<void*>(base), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    for (uint32_t n = base, guard = 0; n && guard < 8192; n += 0x1C, ++guard) {
        if (n + 0x1C > regionEnd) break;
        const uint32_t vt = Read32(n);
        if (vt == 0x00D4D980) continue;
        if (vt != 0x00C66B14) break;
        const uint32_t l = Read32(n + 0x14);
        if (!l || (l >= 0x00400000 && l < 0x010AE000)) continue;
        bool inMaster = false;
        for (int i = 0; i < nmr; ++i) inMaster |= l >= mr[i].base && l < mr[i].base + mr[i].size;
        if (inMaster || std::find(out.begin(), out.end(), l) != out.end()) continue;
        if (!Writable(l, 0x14)) continue;  // once per distinct list
        out.push_back(l);
    }
}
}  // namespace

namespace {
// Tier E (experiment): the fighters' UE3 anim-tree objects. Every UObject is in GObjObjects
// (data [0x00E7C0EC], count [0x00E7C0F0], runtime-verified 2026-10-14); SkelControl* objects
// whose Outer (+0x24) is an AnimTree whose Outer is a fighter's SkeletalMeshComponent (fighter
// +0x10C) changed every frame and were not restored ('unrestored' experiment: +0x70 blend state).
std::vector<uintptr_t> gTierE;
uint32_t gTierEBuiltGf = 0xFFFFFFFF, gTierEComps[2] = {0, 0};
bool ClassStartsWith(uint32_t obj, const char* prefix) {
    char cls[80];
    Ue3Names::NameOf(Read32(obj + 0x30), cls, sizeof cls);
    return !strncmp(cls, prefix, strlen(prefix));
}
// Which fight this is (two-instance tests need both games in the same fight): each fighter's SkeletalMesh asset,
// found as the first pointer in its mesh component (fighter +0x10C) to a live UObject of class SkeletalMesh.
void LogFightImpl(uint32_t gf) {
    char names[2][96] = {"?", "?"};
    for (int i = 0; i < 2; ++i) {
        uint32_t id = 0;
        const uintptr_t obj = CharObj(i, &id);
        const uintptr_t comp = obj ? Read32(obj + 0x10C) : 0;
        if (!comp || !Writable(comp, 0x600)) continue;
        for (uint32_t off = 0x3C; off < 0x600; off += 4) {
            const uint32_t p = Read32(comp + off);
            if (p < 0x01000000 || (p & 3) || !LiveUObject(p)) continue;
            char cls[64];
            Ue3Names::NameOf(Read32(p + 0x30), cls, sizeof cls);
            if (strncmp(cls, "SkeletalMesh", 12) || (cls[12] != '\0' && cls[12] != '_')) continue;
            Ue3Names::NameOf(p, names[i], sizeof names[i]);
            break;
        }
    }
    // The stage the game set for this fight: 0x00811DD0 copies it (or a random pick) into [0x00E89B8C]+0x3C, 64 chars.
    char stage[65] = "?";
    const uint32_t gi = Read32(0x00E89B8C);
    if (gi && Writable(gi + 0x3C, 0x40)) {
        memcpy(stage, reinterpret_cast<const void*>(gi + 0x3C), 64);
        stage[64] = 0;
        for (char* c = stage; *c; ++c)
            if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E) *c = '?';
    }
    Logger::Info("FIGHT at gf=%u: P1 %s, P2 %s, stage %s", gf, names[0], names[1], stage);
}
void BuildTierE(uint32_t gf) {
    uint32_t comps[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        uint32_t id = 0;
        const uintptr_t obj = CharObj(i, &id);
        comps[i] = obj ? Read32(obj + 0x10C) : 0;
    }
    const bool sameFight = comps[0] == gTierEComps[0] && comps[1] == gTierEComps[1];
    // Signed: a replay saves frames from before the build (up to the ring's depth back). As an unsigned difference that
    // was a huge age, and the whole scan ran again inside the replay, the longest frames a session has. A frame far
    // below the build is a new timeline (gf starts over) and still rebuilds.
    const int32_t age = (int32_t)(gf - gTierEBuiltGf);
    if (gTierEBuiltGf != 0xFFFFFFFF && sameFight && age > -64 && age < 600)
        return;
    FramePace::Note("tiere");
    gTierE.clear();
    gTierEBuiltGf = gf;
    gTierEComps[0] = comps[0];
    gTierEComps[1] = comps[1];
    // What each class is, looked up once per class: the scan formatted the class name of every UObject up to three
    // times (MKCamera, MKArenaConstrainPoint, then SkelControl/AnimNode...), every 600 frames inside the live save -
    // one of the two long frames per 600 (net1 2026-11-04: LAN fights with no rollback had 2 frames >= 50 ms per 600).
    // 0 other, 1 camera or arena constraint point, 2 SkelControl, 3 anim tree node. Cleared at a fight change.
    static std::unordered_map<uint32_t, uint8_t> kinds;
    if (!sameFight || kinds.size() > 20000) kinds.clear();
    auto kindOf = [&](uint32_t o) -> uint8_t {
        const uint32_t cls = Read32(o + 0x30);
        const auto it = kinds.find(cls);
        if (it != kinds.end()) return it->second;
        char cn[80];
        Ue3Names::NameOf(cls, cn, sizeof cn);
        auto starts = [&](const char* p) { return !strncmp(cn, p, strlen(p)); };
        const uint8_t k = starts("MKCamera") || starts("MKArenaConstrainPoint") ? 1
                          : starts("SkelControl")                              ? 2
                          : starts("AnimNode") || starts("AnimTree") || starts("MKAnimNode") ? 3
                                                                               : 0;
        kinds.emplace(cls, k);
        return k;
    };
    const uint32_t data = Read32(0x00E7C0EC), count = Read32(0x00E7C0F0);
    if (!data || count > 0x100000 || !Writable(data, count * 4)) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t o = Read32(data + 4 * i);
        if (!o || (o & 3)) continue;
        const uint8_t kind = kindOf(o);
        // The fight camera actor and the arena constraint points (screen/arena edge clamps on fighter
        // positions; both listed as changed-but-unrestored by the load-only diff). 'tierecam 0|1'.
        if (gTierECamera && kind == 1) {
            gTierE.push_back(o);
            continue;
        }
        // SkelControls, and anim tree nodes with 'tierenodes 1' (the class test of the loop's end, done first now).
        if (!(kind == 2 || (gTierENodes && kind == 3))) continue;
        const uint32_t outer = Read32(o + 0x24);
        if (!outer) continue;
        bool owned = outer == comps[0] || outer == comps[1];  // owned by a fighter's component
        // Anim nodes are owned one level down (by the component's AnimTree) - 'tierenodes 0|1'.
        if (!owned && gTierENodes && Writable(outer, 0x28)) {
            const uint32_t outer2 = Read32(outer + 0x24);
            owned = outer2 && (outer2 == comps[0] || outer2 == comps[1]);
        }
        if (!owned) continue;
        // SkelControls only. The component also owns ~38 material instances (render parameters,
        // MaterialInstanceTimeVarying / MwyRevealDecalMaterialInstance); restoring those through the
        // pointer heuristic left a parameter TArray with data 0 and num > 0 (crash 0x007892AB).
        // Anim tree nodes (AnimNodeSequence/Blend*, AnimTree): playback time, weights and root motion state,
        // which drove position drift after loads (GGPO synctest 2026-10-15: most desyncs had no script divergence).
        // (Class test: kindOf above.)
        gTierE.push_back(o);
    }
    Logger::Info("TIERE built at gf=%u: %u fighter anim-tree objects (%u classes known)", gf, (unsigned)gTierE.size(),
                 (unsigned)kinds.size());
    LogFightImpl(gf);
    // What the component actually ticks: root (+0x75C) and the named table (+0x290, 0xC entries, object +8, chain +0x78).
    static int treeLogged = 0;
    for (int i = 0; i < 2 && treeLogged < 2; ++i) {
        const uint32_t comp = comps[i];
        if (!comp || !Writable(comp, 0x760)) continue;
        ++treeLogged;
        char cls[80], out[700];
        int on = 0;
        const uint32_t root = Read32(comp + 0x75C);
        if (root && Writable(root, 0x40)) {
            Ue3Names::NameOf(Read32(root + 0x30), cls, sizeof cls);
            on = AppendF(out, sizeof out, on, "root %08X %s uobj=%d outer=%08X;", root, cls, LiveUObject(root), Read32(root + 0x24));
        }
        const uint32_t arr = Read32(comp + 0x290);
        const int32_t cnt = (int32_t)Read32(comp + 0x294);
        on = AppendF(out, sizeof out, on, " table %d:", cnt);
        for (int32_t k = 0; k < cnt && k < 12 && Writable(arr, cnt * 0xC); ++k) {
            const uint32_t o = Read32(arr + k * 0xC + 8);
            if (!o || !Writable(o, 0x40)) continue;
            Ue3Names::NameOf(Read32(o + 0x30), cls, sizeof cls);
            on = AppendF(out, sizeof out, on, " %s(uobj=%d inE=%d)", cls, LiveUObject(o),
                           std::find(gTierE.begin(), gTierE.end(), o) != gTierE.end());
        }
        Logger::Info("TIERE comp%d %08X: %s", i, comp, out);
    }
    static int listed = 0;
    if (listed++ < 4) {
        std::map<std::string, int> classes;
        for (uintptr_t o : gTierE) {
            char cls[80];
            Ue3Names::NameOf(Read32(o + 0x30), cls, sizeof cls);
            uint32_t info[2] = {0, 0};
            LiveBlockInfo(reinterpret_cast<const void*>(o), info);
            char key[120];
            snprintf(key, sizeof key, "%s (block %u)", cls, info[1]);
            ++classes[key];
        }
        for (auto& [k, n] : classes) Logger::Info("TIERE   %3d x %s", n, k.c_str());
    }
}
}  // namespace

bool gCineWatch = false;  // (inside namespace Snapshot, opened above)
namespace {
struct CineObj {
    uint32_t idx = 0, size = 0, seenGf = 0;
    uint32_t chunk[64] = {};  // FNV-1a of each 64-byte chunk of the body from +0x3C (up to 4 KB)
};
std::unordered_map<uint32_t, bool> gCineClass;   // UClass -> its name contains "Cinema"
std::unordered_map<uint32_t, CineObj> gCineObjs;  // tracked object -> last state
uint32_t gCineLines = 0;
uint32_t gCineFighter[2][9];  // the fighter fields CineWatch logs on change (offs below)
uint32_t ChunkHash(uintptr_t p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ reinterpret_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}
void ClassName(uint32_t obj, char* out, size_t n) {
    out[0] = 0;
    if (obj && Writable(obj, 0x40)) Ue3Names::NameOf(Read32(obj + 0x30), out, n);
}
}  // namespace

void CineWatch(uint32_t gf, const uint32_t actors[2]) {
    if (!gCineWatch || gCineLines > 6000) return;
    const uint32_t data = Read32(0x00E7C0EC), count = Read32(0x00E7C0F0);
    if (!data || count > 0x100000 || !Writable(data, count * 4)) return;
    int lines = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t o = Read32(data + 4 * i);
        if (!o || (o & 3) || !Writable(o, 0x40)) continue;
        const uint32_t cls = Read32(o + 0x30);
        auto c = gCineClass.find(cls);
        if (c == gCineClass.end()) {
            char name[96];
            Ue3Names::NameOf(cls, name, sizeof name);
            c = gCineClass.emplace(cls, strstr(name, "Cinema") != nullptr).first;
        }
        if (!c->second) continue;
        uint32_t info[2] = {0, 0};
        if (!LiveBlockInfo(reinterpret_cast<const void*>(o), info)) continue;
        const uint32_t size = info[1] > 0x1000 + 0x3C ? 0x1000 + 0x3C : info[1];
        auto it = gCineObjs.find(o);
        const bool born = it == gCineObjs.end() || it->second.idx != i;
        CineObj& t = gCineObjs[o];
        char cls1[80], obj1[80], out1[80];
        if (born) {
            t = CineObj{};
            t.idx = i;
            t.size = size;
            ClassName(o, cls1, sizeof cls1);
            Ue3Names::NameOf(o, obj1, sizeof obj1);
            const uint32_t outer = Read32(o + 0x24);
            Ue3Names::NameOf(outer, out1, sizeof out1);
            Logger::Frame("CINE gf=%u BIRTH %08X idx %u %s '%s' block %u, outer %08X '%s'", gf, o, i, cls1, obj1, info[1], outer, out1);
            ++gCineLines;
        }
        t.seenGf = gf;
        if (size <= 0x3C) continue;
        char changed[200] = "";
        int k = 0, nchg = 0;
        for (uint32_t c64 = 0; c64 < 64 && 0x3C + c64 * 64 < size; ++c64) {
            const uint32_t off = 0x3C + c64 * 64, len = size - off < 64 ? size - off : 64;
            const uint32_t h = ChunkHash(o + off, len);
            if (!born && h != t.chunk[c64]) {
                ++nchg;
                if (k < (int)sizeof changed - 12) k = AppendF(changed, sizeof changed, k, " +%X", off);
            }
            t.chunk[c64] = h;
        }
        if (nchg && lines < 12) {
            ClassName(o, cls1, sizeof cls1);
            Logger::Frame("CINE gf=%u CHANGE %08X %s: %d chunk(s)%s", gf, o, cls1, nchg, changed);
            ++lines;
            ++gCineLines;
        }
    }
    for (auto it = gCineObjs.begin(); it != gCineObjs.end();) {
        if (it->second.seenGf != gf) {
            Logger::Frame("CINE gf=%u DEATH %08X idx %u", gf, it->first, it->second.idx);
            ++gCineLines;
            it = gCineObjs.erase(it);
        } else {
            ++it;
        }
    }
    // The fighters (MKPawn, docs/mkpawn_layout.md): MotionControl slots +0x13C..+0x148, the refresh target +0x1C8,
    // Physics (byte +0x58), Base +0x60, the bool word +0xA8 (bIgnoreForDesyncs), CinemaControlCount +0x138.
    static const uint32_t offs[9] = {0x13C, 0x140, 0x144, 0x148, 0x1C8, 0x58, 0x60, 0xA8, 0x138};
    for (int p = 0; p < 2; ++p) {
        const uint32_t a = actors[p];
        if (!a || !Writable(a, 0x200)) continue;
        uint32_t v[9];
        bool diff = false;
        for (int j = 0; j < 9; ++j) {
            v[j] = Read32(a + offs[j]);
            if (j == 5) v[j] &= 0xFF;
            diff |= v[j] != gCineFighter[p][j];
        }
        if (!diff) continue;
        char vt[4][16];
        for (int j = 0; j < 4; ++j) snprintf(vt[j], sizeof vt[j], "%08X", v[j] && Writable(v[j], 4) ? Read32(v[j]) : 0);
        Logger::Frame("CINE gf=%u P%d slots %08X(vt %s) %08X(vt %s) %08X(vt %s) %08X(vt %s) +1C8 %08X Physics %u Base %08X +A8 %08X "
                      "CinemaControlCount %d",
                      gf, p + 1, v[0], vt[0], v[1], vt[1], v[2], vt[2], v[3], vt[3], v[4], v[5], v[6], v[7], (int)v[8]);
        ++gCineLines;
        memcpy(gCineFighter[p], v, sizeof v);
    }
}

namespace {
std::unordered_set<const StateAB*> gStates;  // every snapshot SaveAB filled (diagnostics only)
bool InOwnBuffers(uintptr_t p) {
    for (const StateAB* s : gStates)
        for (const auto& b : s->bytes)
            if (!b.empty() && p >= reinterpret_cast<uintptr_t>(b.data()) && p < reinterpret_cast<uintptr_t>(b.data()) + b.size())
                return true;
    return false;
}
}  // namespace

void WriteOwnRanges(FILE* f) {
    for (const StateAB* s : gStates)
        for (const auto& b : s->bytes)
            if (!b.empty())
                fprintf(f, "%08X %08X\n", (unsigned)reinterpret_cast<uintptr_t>(b.data()),
                        (unsigned)(reinterpret_cast<uintptr_t>(b.data()) + b.size()));
}

// The five MK heaps' spinlocks ([heap+0x30], taken with lock cmpxchg 0->1 by heap Free 0x0042B1D0 / Alloc):
// held while the heap headers and the master image are copied or restored, so that no other thread's
// allocation tears the image (hypothesis for the 0x0042AF1D free-tree crashes whose rate followed save
// timing, 2026-10-15). Command 'heaplock 0|1'.
bool gHeapLock = true;
struct HeapLocks {
    uint32_t held[8] = {};
    int n = 0;
    HeapLocks() {
        if (!gHeapLock) return;
        for (uintptr_t a = 0x00ED53F0; a <= 0x00ED5400; a += 4) {
            const uint32_t h = Read32(a);
            if (!h || !Writable(h + 0x30, 4)) continue;
            volatile LONG* lk = reinterpret_cast<volatile LONG*>(h + 0x30);
            bool got = false;
            for (int spin = 0; spin < 200000 && !got; ++spin) {
                got = InterlockedCompareExchange(lk, 1, 0) == 0;
                if (!got) YieldProcessor();
            }
            if (got) held[n++] = h;
            else Logger::Info("HEAPLOCK: heap %08X stayed locked; copying without it", h);
        }
        // GMalloc's medium heap (same class, not restored): content spans live in it, and LoadAB checks them against
        // its free tree, which must not change meanwhile.
        volatile LONG* mlk = reinterpret_cast<volatile LONG*>(0x00E0AC18 + 0x30);
        bool got = false;
        for (int spin = 0; spin < 200000 && !got; ++spin) {
            got = InterlockedCompareExchange(mlk, 1, 0) == 0;
            if (!got) YieldProcessor();
        }
        if (got) held[n++] = 0x00E0AC18;
    }
    ~HeapLocks() {
        for (int i = 0; i < n; ++i) InterlockedExchange(reinterpret_cast<volatile LONG*>(held[i] + 0x30), 0);
    }
};
int64_t gProf[12] = {};
int gProfSaves = 0;

void LogFight(uint32_t gf) { LogFightImpl(gf); }
bool TierACovers(uint32_t addr, uint32_t size) {
    for (const Raw& r : TierA())
        if (addr >= r.base && addr + size <= r.base + r.size) return true;
    return false;
}

// Diagnostic (deep diff): the GMalloc blocks the snapshot captures as xblocks for MK anim nodes and channels.
int ExternalBlocks(std::vector<std::pair<uintptr_t, uint32_t>>& out) {
    MKHeap::Region heap{};
    out.clear();
    if (!MKHeap::Discover(&heap)) return 0;
    FindExternalBlocks(heap.base, heap.size, out);
    return (int)out.size();
}

bool SaveAB(StateAB* out, uint32_t gameFrame, char* why, size_t whyLen) {
    struct ProbeFastOff {  // never leave the save with guarded-read probing on: LoadAB's Writable must query
        ~ProbeFastOff() { gProbeFast = false; }
    } probeFastOff;
    gScanN = 0;  // fresh region cache for this save
    if (gTrimWsMs > 0) {  // test aid ('trimws N'): the working set emptied as Windows trims a minimized game's
        static ULONGLONG lastTrim = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastTrim >= static_cast<ULONGLONG>(gTrimWsMs)) {
            lastTrim = now;
            K32EmptyWorkingSet(GetCurrentProcess());
        }
    }
    static int savesSinceReset = 0;
    if (++savesSinceReset >= 120 || !gPositiveCache) {
        gCommittedN = 0;
        savesSinceReset = 0;
    }
    static int savesSinceModules = 0;
    if (gModSections.empty() || ++savesSinceModules >= 600) {  // modules can load late (PhysX on first use)
        if (gForeignKeeps) Logger::Info("FOREIGNVT: %u pointer dwords to other modules' objects kept live in 600 saves", gForeignKeeps);
        gForeignKeeps = 0;
        if (gColdKeeps)
            Logger::Info("COLDKEEP: %u pointer-like dwords kept live unread (target page not in the working set) in %u scans; "
                         "%u page(s) handed to the warming thread", gColdKeeps, gColdScans, gWarmQueued);
        gColdKeeps = gColdScans = gWarmQueued = 0;
        savesSinceModules = 0;
        FramePace::Note("modsections");
        BuildModSections();
    }
    if (gameFrame % 600 == 0) FramePace::Note("spanaudit");
    gStates.insert(out);
    MemQuery::Scope memScope;
    if ((int32_t)Read32(MKHeap::kScriptDepth) != -1) {
        snprintf(why, whyLen, "script VM not idle (depth %d)", (int)Read32(MKHeap::kScriptDepth));
        return false;
    }
    MKHeap::Region heap{};
    if (!MKHeap::Discover(&heap)) {
        snprintf(why, whyLen, "object heap not discovered");
        return false;
    }
    out->gameFrame = gameFrame;
    out->idCounter = Read32(MKHeap::kObjectIdCounter);
    // Reuse span/buffer storage: this runs every frame in rollback mode. The span order changes between saves,
    // so buffers are matched by size (best fit from this slot's previous buffers), not by index: by index every
    // buffer ratcheted to the largest span ever stored at its position (+180 MB per 1200 SyncTest frames,
    // synctest92), and dropping oversized ones instead reallocated megabytes per save (21 ms saves, synctest96).
    std::multimap<size_t, std::vector<uint8_t>> pool;
    for (auto& b : out->bytes)
        if (b.capacity()) pool.emplace(b.capacity(), std::move(b));
    out->bytes.clear();
    size_t n = 0;
    auto add = [&](const char* name, uintptr_t base, uint32_t size, std::vector<uint32_t>&& keep) {
        if (out->spans.size() <= n) out->spans.emplace_back();
        {
            auto it = pool.lower_bound(size);
            if (it != pool.end()) {
                out->bytes.push_back(std::move(it->second));
                pool.erase(it);
            } else {
                out->bytes.emplace_back();
            }
        }
        Span& s = out->spans[n];
        s.name = name;
        s.base = base;
        s.size = size;
        s.identity = 0;
        s.keepOffsets = std::move(keep);
        std::vector<uint8_t>& b = out->bytes[n];
        // A heap block the game already freed (held by the quarantine) is dead: a stale MK pointer can still lead the
        // xblock walk to it, and restoring it later wrote into a released block (forcelod A/B run: "written after
        // free" on an 8408-byte block freed by 0x004D2E96, then the medium free tree broke at 0x0042ACFD). The span
        // stays (callers set its identity right after add()) but is empty, so nothing is restored.
        const uintptr_t blk = !strcmp(name, "ue3anim") || !strcmp(name, "fxactor") || !strcmp(name, "mkactor") ? base - 0x3C : base;
        if (blk >= 0x01000000 && strcmp(name, "mkmaster") && FreeQuarantine::IsPending(reinterpret_cast<const void*>(blk))) {
            ++gPendingSkips;
            s.size = 0;
            b.clear();
            ++n;
            return;
        }
        // Span-size audit (every 600 saves): a heap span must not run past its GMalloc block (the fighter actor span
        // covered 0x40 bytes of the next slot until 2026-10-17). Logged once per span name.
        if (gameFrame % 600 == 0 && blk >= 0x01000000 && strcmp(name, "mkmaster") && !(blk & 7)) {
            static std::unordered_set<std::string> reported;  // name@base, at most a few per name
            static std::map<std::string, int> perName;
            uint32_t info[2] = {0, 0};
            char key[96];
            snprintf(key, sizeof key, "%s@%08X", name, (unsigned)base);
            if (CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(blk), info) && info[1] >= 8 && info[1] <= 0x10000000 &&
                base + size > blk + info[1] && perName[name] < 4 && reported.insert(key).second && ++perName[name])
                Logger::Info("SPANAUDIT %s [%08X,+%X) runs past its GMalloc block %08X (size %X) by %X bytes", name, (unsigned)base,
                             size, (unsigned)blk, info[1], (unsigned)(base + size - blk - info[1]));
        }
        b.resize(size);
        // The master image (11.8 MB, ~1.2 ms of every save on one core) is copied with the helper threads (rc23); spans
        // under 256 KB as before. Into the slot with non-temporal stores from 64 KB up (rc25; the same bytes).
        ParallelMem::CopyToSlot(b.data(), reinterpret_cast<const void*>(base), size);
        ++n;
    };
    // Save cost profile (phases 1..5, accumulated QPC ticks, printed every 600 saves).
    LARGE_INTEGER tPrev;
    QueryPerformanceCounter(&tPrev);
    auto Mark = [&](int phase) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        gProf[phase] += t.QuadPart - tPrev.QuadPart;
        tPrev = t;
    };
    // Tier B: the whole MK master heap except the asset sub-heaps (includes the object heap).
    MKHeap::Region mr[8];
    const int nmr = MKHeap::MasterRanges(mr, 8);
    if (nmr <= 0) {
        snprintf(why, whyLen, "master heap ranges not found");
        return false;
    }
    {
        HeapLocks locks;  // heap headers (Tier A) and the master image (Tier B) as one consistent copy
        for (const Raw& r : TierA()) add(r.name, r.base, r.size, {});
        Mark(1);
        for (int i = 0; i < nmr; ++i) add("mkmaster", mr[i].base, mr[i].size, {});
    }
    Mark(2);
    // rc25: guarded-read probing from here on (it was turned on at the skel phase): the extlists and actors phases' Writable
    // checks only decide which spans are copied (VirtualQuery each in the extlists walk, uncached: 'memcache' is off).
    // LoadAB checks every span with VirtualQuery before it writes. 'fastprobe 0' turns it off with the rest.
    gProbeFast = gFastProbe && CrashLog::SafeReadAvailable();
    static std::vector<uintptr_t> extLists;
    ExternalLists(mr, nmr, extLists);
    for (uintptr_t l : extLists) add("extlist", l, 0x14, {});
    Mark(3);
    // Tier C: external array blocks owned by MK objects (restored in place).
    static std::vector<std::pair<uintptr_t, uint32_t>> blocks;
    FindExternalBlocks(heap.base, heap.size, blocks);
    for (auto& b : blocks) add("xblock", b.first, b.second, {});
    Mark(4);
    // UE3 objects holding raw MK-object pointers (Ue3Refs): their holder cells.
    static std::vector<std::pair<uintptr_t, uint32_t>> cells;
    cells.clear();
    Ue3Refs::CollectCells(cells);
    for (auto& c : cells) add("ue3cell", c.first, c.second, {});
    Mark(5);
    // Tier C (partial): the two fighters' UE3 actors, pointer-safe merge.
    for (int i = 0; i < 2; ++i) {
        uint32_t id = 0;
        const uintptr_t obj = CharObj(i, &id);
        const uintptr_t actor = obj ? Read32(obj + 0x08) : 0;
        // Fail closed on another actor class: its own PropertiesSize must cover the span.
        const uint32_t cls = actor && Writable(actor, 0x34) ? Read32(actor + 0x30) : 0;
        if (actor && Writable(actor, kActorSpan) && cls && Writable(cls, 0x44) && Read32(cls + 0x40) >= kActorSpan &&
            Read32(cls + 0x40) <= 0x400)
        {
            if (gameFrame % 97 == 0) VtableKeepReport(i ? "actor1" : "actor0", actor, kActorSpan);
            add(i ? "actor1" : "actor0", actor, kActorSpan, PointerLikeOffsets(actor, kActorSpan));
        }
    }
    Mark(6);
    // Sub-profile of the "skel" phase (2.8-5.3 ms/save on Courtyard/CourtyardN, 2026-11-04 sweep): QPC ticks and
    // VirtualQuery calls per part, printed with SAVE PROFILE.
    static int64_t skelProf[10] = {};
    static uint32_t skelVq = 0, saveVq = 0, skelCounts[4] = {};
    const uint32_t vqAtSkel = MemQuery::gVqCalls;
    gProbeFast = gFastProbe && CrashLog::SafeReadAvailable();  // until Mark(7): reads only (see ProbeRead)
    LARGE_INTEGER skelPrev = tPrev;
    auto Sub = [&](int k) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        skelProf[k] += t.QuadPart - skelPrev.QuadPart;
        skelPrev = t;
    };
    // Actors attached to the fighters (effects on bones, props; AttachedActors actor+0x68, 0x60-byte records, actor
    // first): not in any other span, so after a load an effect attached at S kept the abandoned future's state
    // (attachment, timers) and the replay's scripts saw it (imgtrace2 2026-10-17: load 6547, AttachedActors 3 at
    // the save / 2 live, the first script call differed). Restored like Tier E objects from +0x3C (identity, block,
    // reflection data, pointers kept live); 'abtest fxactors' toggles the load.
    static std::unordered_set<uint32_t> fxSeen;  // actors already saved by a span below
    fxSeen.clear();
    if (gFxActors) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0, id2 = 0;
            const uintptr_t obj = CharObj(i, &id), other = CharObj(1 - i, &id2);
            const uintptr_t actor = obj ? Read32(obj + 0x08) : 0, otherActor = other ? Read32(other + 0x08) : 0;
            if (!actor || !Writable(actor + 0x68, 12)) continue;
            const uint32_t data = Read32(actor + 0x68), num = Read32(actor + 0x6C);
            if (!data || num > 16 || !Writable(data, num * 0x60)) continue;
            for (uint32_t k = 0; k < num; ++k) {
                const uint32_t a = Read32(data + k * 0x60);
                if (!a || a == otherActor || a == actor || !LiveUObject(a) || (Read32(a + 0x80) & 0x20)) continue;
                const uint32_t cls = Read32(a + 0x30);
                uint32_t info[2] = {0, 0};
                if (!cls || !Writable(cls, 0x44) || !LiveBlockInfo(reinterpret_cast<const void*>(a), info)) continue;
                uint32_t size = Read32(cls + 0x40);
                if (size > info[1]) size = info[1];
                if (size > 0x800) size = 0x800;
                if (size <= 0x40 || !Writable(a, size)) continue;
                add("fxactor", a + 0x3C, size - 0x3C, PointerLikeOffsets(a + 0x3C, size - 0x3C));
                out->spans[n - 1].identity = Read32(a + 4);
                fxSeen.insert(a);
                ++gFxActorSaved;
                ++skelCounts[0];
            }
        }
    }
    Sub(0);
    // The fighters' move-request entries (fighter+0x7524, up to 50 pointers, count +0x75EC; 0x248-byte records, dispatched
    // by 0x008AB6F0 on type +0x70). They live in the character's loaded data (0x429xxxxx/0x473xxxxx, no GMalloc block),
    // which no span covered, but carry per-fight state: +0x04 is 1 for P1's entries and 0 for P2's and flipped on a side
    // switch, so a replay kept the abandoned future's side and missed a special move live made (movereq_1/2 2026-10-18:
    // all entries' +0x04 differed on whole frames; at gf 1130 the replay ran 4 more entries; P2 meter 0.0125 apart).
    // Raw restore ('abtest movereq').
    if (gMoveReqs) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            if (!obj || !Writable(obj + 0x7524, 0xCC)) continue;
            const uint32_t cnt = Read32(obj + 0x75EC);
            for (uint32_t k = 0; k < cnt && k < 50; ++k) {
                const uint32_t e = Read32(obj + 0x7524 + 4 * k);
                if (!e || !Writable(e, kMoveReqSize)) continue;
                add("movereq", e, kMoveReqSize, {});
                out->spans[n - 1].identity = (uint32_t)obj;
            }
        }
    }
    Sub(1);
    // Progressive costume damage: the fighter's SkeletalMeshComponent (fighter +0x10C) holds groups at +0x570 (count
    // +0x574; 16-byte records, data +4, count +8) of 0x14-byte damage pieces with a "used" flag at +0x04. When the
    // fighter's per-region damage (+0x4D8[idx], 0x00447240) reaches the next level, 0x00493F80 draws the rng among up to
    // 4 unused pieces and marks the pick used. The pieces were in no span, so a replay saw the abandoned future's picks
    // as used, had no candidate and skipped the draw (clean_1 2026-10-18: both failing loads, RNGTRACE "original 2
    // draws (caller 0x0044729E), resim 1"). Raw restore while the group still owns the block ('abtest costdmg').
    if (gCostDmg) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            const uintptr_t comp = obj ? Read32(obj + 0x10C) : 0;
            if (!comp || !Writable(comp + 0x570, 8)) continue;
            const uint32_t groups = Read32(comp + 0x570), ng = Read32(comp + 0x574);
            if (!groups || ng > 32 || !Writable(groups, ng * 16)) continue;
            for (uint32_t g = 0; g < ng; ++g) {
                const uint32_t data = Read32(groups + 16 * g + 4), cnt = Read32(groups + 16 * g + 8);
                if (!data || !cnt || cnt > 64 || !Writable(data, cnt * 0x14)) continue;
                add("costdmg", data, cnt * 0x14, {});
                out->spans[n - 1].identity = groups + 16 * g + 4;  // the cell that must still point at the block
            }
        }
    }
    Sub(2);
    // UE3 actors of the other MK objects (obj+0x08: projectiles, effects, props not attached to a fighter): in
    // no other span, so a replay started from the abandoned future's Location. The hit test 0x008A46E0 reads it
    // (0x00433460: actor +0xDC/+0xE0/+0xE4) and hit in the replay only (hit_1 2026-10-18, load 1100: Y 60 units
    // = 4 frames x 15 ahead on every replayed frame, then the replay hit and started script 0xCE). Restored like
    // fxactor spans ('abtest mkactors').
    // One walk of the global MK object list for the three users below (MK actors, MK helpers, prop skeletal components):
    // it ran three times per save, TypeOf and strstr on every listed object each time (the "skel" save phase, 0.75 ms).
    static std::vector<uint32_t> mkActors, propComps, skelObjs;
    static std::vector<std::pair<uint32_t, uint32_t>> helpers;
    mkActors.clear();
    propComps.clear();
    skelObjs.clear();
    helpers.clear();
    if (gMkActors || gMkHelpers || (gPropSkel && gSkelArrays) || gNameMaps) {
        static bool wantActors, wantHelpers, wantComps, wantMaps;
        wantActors = gMkActors;
        wantHelpers = gMkHelpers;
        wantComps = gPropSkel && gSkelArrays;
        wantMaps = gNameMaps;
        ObjectHooks::ForEachListed([](uint32_t obj, const char* type, void*) {
            if (!type) return;
            if (strstr(type, "MKObj")) {
                uint32_t a = 0, h = 0, vt = 0;
                if (wantActors && CrashLog::SafeRead32(obj + 0x08, &a) && a) mkActors.push_back(a);
                if (wantHelpers && CrashLog::SafeRead32(obj + 0x1D8, &h) && h && !(h & 3) && CrashLog::SafeRead32(h, &vt) &&
                    vt == kMkHelperVtable)
                    helpers.push_back({obj, h});
            }
            if ((wantComps || wantMaps) && strstr(type, "MKSkeletalObj")) {
                uint32_t comp = 0;
                if (wantComps && CrashLog::SafeRead32(obj + 0x10C, &comp) && comp) propComps.push_back(comp);
                if (wantMaps) skelObjs.push_back(obj);
            }
        }, nullptr);
    }
    Sub(3);
    skelCounts[1] += (uint32_t)mkActors.size();
    skelCounts[2] += (uint32_t)helpers.size();
    skelCounts[3] += (uint32_t)propComps.size();
    if (gMkActors) {
        auto addMk = [&](uint32_t a) {
            if (!LiveUObject(a) || (Read32(a + 0x80) & 0x20)) return;
            const uint32_t cls = Read32(a + 0x30);
            uint32_t info[2] = {0, 0};
            if (!cls || !Writable(cls, 0x44) || !LiveBlockInfo(reinterpret_cast<const void*>(a), info)) return;
            uint32_t size = Read32(cls + 0x40);
            if (size > info[1]) size = info[1];
            if (size > 0x800) size = 0x800;
            if (size <= 0xF0 || !Writable(a, size)) return;  // must cover Location/Rotation
            add("mkactor", a + 0x3C, size - 0x3C, PointerLikeOffsets(a + 0x3C, size - 0x3C));
            out->spans[n - 1].identity = Read32(a + 4);
            ++gMkActorSaved;
        };
        uint32_t id = 0;
        for (int i = 0; i < 2; ++i) {
            const uintptr_t obj = CharObj(i, &id);
            if (obj) fxSeen.insert(Read32(obj + 0x08));
        }
        for (uint32_t a : mkActors)
            if (fxSeen.insert(a).second) addMk(a);
        // The Base an MK actor follows when it is in no span of its own: traced soak 13116 S=1654 (2026-11-05), a hit box
        // (MK object 13600000's actor) based on MKParticleActor_97 with a zero offset. The particle actor is neither a
        // fighter's attached actor nor an MK object's +0x08, so the load left it at the abandoned future's position; on
        // the first replayed frame the hit box's attachment controller (MotionControl +0x13C, 0x007853F0 -> 0x0078D1B0 ->
        // MoveActor) pulled the restored hit box onto it, 4 frames of travel ahead (LOCMOVED on every load of that
        // flight), and the original's hit at gf 1655 never came in the replay. Saved like the MK actors.
        if (gMkBases) {
            const size_t nActors = mkActors.size();
            for (size_t k = 0; k < nActors; ++k) {
                uint32_t b = 0;
                if (!CrashLog::SafeRead32(mkActors[k] + 0x60, &b) || !b || (b & 3) || !fxSeen.insert(b).second) continue;
                const auto before = n;
                addMk(b);
                if (n == before) continue;
                if (gMkBaseSaved++ < 10) {
                    char cls[64] = "?";
                    Ue3Names::NameOf(Read32(b + 0x30), cls, sizeof cls);
                    Logger::Info("MKBASE: %08X (%s), the base of MK actor %08X, saved as an mkactor span (%u so far)", b, cls, mkActors[k],
                                 gMkBaseSaved);
                } else if (gMkBaseSaved % 5000 == 0) {
                    Logger::Info("MKBASE: %u base actor spans saved", gMkBaseSaved);
                }
            }
        }
    }
    Sub(4);
    // The 0x4C-byte GMalloc helper an MK object keeps at +0x1D8 (vtable 0x00C6A5B4, made at 0x0089F2CE): outside the MK
    // image, so its fields were the abandoned future's. Its +0xC gates the object's pending-script spawn in the tick
    // 0x008A7300 (-> 0x0089F7F0): VMTRACE soak 11804 S=2115, tickgate [+0x1D8]+0xC 0 in the original, 1 in the replay,
    // then a script VM and a prop spawned in the replay only (the dominant remaining failing-load class). Saved with
    // the pointer-safe merge (its pointer fields stay live); identity = the owning object ('abtest mkhelper').
    if (gMkHelpers) {
        for (const auto& oh : helpers) {
            uint32_t info[2] = {0, 0};
            if (!Writable(oh.second, kMkHelperSize) || !LiveBlockInfo(reinterpret_cast<const void*>(oh.second), info) ||
                info[1] < kMkHelperSize)
                continue;
            add("mkhelper", oh.second, kMkHelperSize, PointerLikeOffsets(oh.second, kMkHelperSize));
            out->spans[n - 1].identity = oh.first;
            ++gMkHelperSaved;
        }
    }
    Sub(5);
    // Fighter SkeletalMeshComponent (fighter +0x10C, vtable 0x00D5A388) bone arrays: per-frame
    // written, pure data (runtime 2026-10-14: 189 bones; +0x2C8 x40 B, +0x2F4/+0x300/+0x30C
    // x64 B matrices). Only the data blocks are restored, and only while the component still
    // owns the same block (identity = TArray header address, checked at load).
    if (gSkelArrays) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            const uintptr_t comp = obj ? Read32(obj + 0x10C) : 0;
            if (!comp || !Writable(comp, 0x320) || Read32(comp) != kSkelMeshVtable) continue;
            for (const SkelArray& a : kSkelArrays) {
                const uintptr_t data = Read32(comp + a.offset);
                const uint32_t num = Read32(comp + a.offset + 4);
                if (!data || num == 0 || num > 1024 || !Writable(data, num * a.elem)) continue;
                add(a.name, data, num * a.elem, {});
                out->spans[n - 1].identity = (uint32_t)(comp + a.offset);
            }
        }
    }
    // The other MK skeletal objects' components (MKSkeletalObj +0x10C: props and meshes attached to a fighter). MK
    // objects based on them re-place their actor every frame in MKObj::Update (0x0043E108..0x0043E17C: relative
    // location 0x007896F0 + the frame's delta -> SetRelativeLocation 0x00727C40 -> world from the base's bone matrix
    // comp +0x2F4 x LocalToWorld comp +0x70). A load restored the attached actor but not this pose, so the first
    // replayed update re-placed it from the abandoned future's pose: soak 13101-13112, 75 LOCMOVED replays with an extra
    // write through 0x0043E14F (3 originals), the hit box a frame ahead (13011 S=2063). Saved: the bone arrays (as the
    // fighters'), the three matrix-array headers (their rotation, "proprot") and LocalToWorld ("propl2w").
    if (gPropSkel && gSkelArrays) {
        std::vector<uint32_t>& comps = propComps;
        uint32_t fighterComp[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            fighterComp[i] = obj ? Read32(obj + 0x10C) : 0;
        }
        std::sort(comps.begin(), comps.end());
        comps.erase(std::unique(comps.begin(), comps.end()), comps.end());
        for (const uint32_t comp : comps) {
            if (comp == fighterComp[0] || comp == fighterComp[1] || !Writable(comp, 0x320) || Read32(comp) != kSkelMeshVtable) continue;
            for (const SkelArray& a : kPropSkelArrays) {
                const uintptr_t data = Read32(comp + a.offset);
                const uint32_t num = Read32(comp + a.offset + 4);
                if (!data || num == 0 || num > 1024 || !Writable(data, num * a.elem)) continue;
                add(a.name, data, num * a.elem, {});
                out->spans[n - 1].identity = (uint32_t)(comp + a.offset);
            }
            add("proprot", comp + 0x2F4, 0x24, {});
            out->spans[n - 1].identity = comp;
            add("propl2w", comp + 0x70, 0x40, {});
            out->spans[n - 1].identity = comp;
            ++gPropSkelSaved;
        }
    }
    Sub(6);
    // The fighter SkeletalMeshComponent itself (from +0x3C, pointer-safe merge like the actors): script
    // natives read its scalar fields, e.g. 0x008CDE00 -> 0x00447510 reads +0x148 (a per-frame ramp that
    // carried over from the abandoned timeline; GGPO synctest 2026-10-15). Command 'compspan 0|1'.
    // The fighters' other components (Components TArray actor+0x3C: collision cylinder etc.): their cached
    // transforms/bounds feed UE3 movement collision. After a load the actor's Location is restored but the
    // components kept the live (future) transform until the actor moved again, and the first replayed frame
    // drifted (GGPO synctest 2026-10-15: MK position = actor Location via 0x00433410). 'pawncomps 0|1'.
    if (gPawnComps) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            const uintptr_t actor = obj ? Read32(obj + 0x08) : 0;
            const uintptr_t mesh = obj ? Read32(obj + 0x10C) : 0;
            if (!actor || !Writable(actor + 0x3C, 12)) continue;
            const uint32_t data = Read32(actor + 0x3C), num = Read32(actor + 0x40);
            if (!data || num > 64 || !Writable(data, num * 4)) continue;
            for (uint32_t k = 0; k < num; ++k) {
                const uint32_t c = Read32(data + 4 * k);
                if (!c || c == mesh || !LiveUObject(c)) continue;
                uint32_t info[2] = {0, 0};
                if (!LiveBlockInfo(reinterpret_cast<const void*>(c), info)) continue;
                const uint32_t size = info[1] > 0x800 ? 0x800 : info[1];
                if (size <= 0x40 || !Writable(c, size)) continue;
                add("ue3anim", c + 0x3C, size - 0x3C, PointerLikeOffsets(c + 0x3C, size - 0x3C));
                out->spans[n - 1].identity = Read32(c + 4);  // GObjObjects index, re-checked at load
            }
        }
    }
    Sub(7);
    if (gCompSpan) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            const uintptr_t comp = obj ? Read32(obj + 0x10C) : 0;
            uint32_t info[2] = {0, 0};
            if (!comp || Read32(comp) != kSkelMeshVtable || !LiveBlockInfo(reinterpret_cast<const void*>(comp), info)) continue;
            const uint32_t size = info[1] > 0x1000 ? 0x1000 : info[1];
            if (size <= 0x40 || !Writable(comp, size)) continue;
            // The component's queued anim-object list (TArray +0x3A8, appended by 0x76DF10, cleared at 0x77B6xx): the
            // merge keeps the {data, num, max} triple live, so a replay saw the abandoned future's count (BONEHASH
            // synctest38: num 8 vs 10). Its contents are captured here; the merge restores num when the block is the same.
            if (gCompList) {
                const uint32_t ld = Read32(comp + 0x3A8), ln = Read32(comp + 0x3AC);
                if (ld && ln > 0 && ln <= 256 && Writable(ld, ln * 4)) {
                    add("complist", ld, ln * 4, {});
                    out->spans[n - 1].identity = (uint32_t)(comp + 0x3A8);
                }
            }
            if (gameFrame % 97 == 0) VtableKeepReport("fightercomp", comp + 0x3C, size - 0x3C);
            add("fightercomp", comp + 0x3C, size - 0x3C, PointerLikeOffsets(comp + 0x3C, size - 0x3C));
        }
    }
    // The fighters' name-keyed lookup cache (TMap at MK object +0x138, filled lazily by 0x456F30 -> 0x443C10):
    // the header is in the MK image, its element (28-byte) and hash blocks are GMalloc blocks. Restoring the
    // header against the live blocks left a hash chain with a cycle (synctest40 hang at 0x00443BCC).
    // Every MK skeletal object has this map (0x00444B20 / 0x00444E90 look names up in obj +0x138 next to the component
    // obj +0x10C), props too: their headers came back with the image over the abandoned future's blocks, the original
    // synctest40 cycle. Traced soak 13119 (2026-11-05, smoke v Scorpion, Living Forest) hung in that lookup at gf 2475
    // with element 2 linked to itself. Saved for all of them, with (object, id) pairs for the load's decision.
    out->mapObjs.clear();
    if (gNameMaps) {
        static std::vector<uint32_t> objs;
        objs.assign(skelObjs.begin(), skelObjs.end());
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            if (const uintptr_t obj = CharObj(i, &id)) objs.push_back((uint32_t)obj);
        }
        std::sort(objs.begin(), objs.end());
        objs.erase(std::unique(objs.begin(), objs.end()), objs.end());
        // Map layout (0x00443B50 find, 0x00443C10 add -> 0x004B74D0 sparse add): elements TArray +0x138/+0x13C/+0x140
        // (0x1C each, hash next +0x14), free-index stack TArray of ints +0x144/+0x148/+0x14C (an add pops its last
        // entry), allocation bits: overflow block +0x150 (inline at +0x15C while 128 bits fit), hash block +0x16C, size
        // +0x170 (a power of two). Without the stack and bits blocks, a load left the restored free count over the
        // abandoned future's stack: the next add could pop an index still linked in a chain.
        static uint32_t sRejected = 0, sAccepted = 0;
        for (const uint32_t obj : objs) {
            if (!obj || !Writable(obj + 0x138, 0x3C)) continue;
            const uint32_t data = Read32(obj + 0x138), num = Read32(obj + 0x13C), max = Read32(obj + 0x140);
            const uint32_t fdata = Read32(obj + 0x144), fnum = Read32(obj + 0x148), fmax = Read32(obj + 0x14C);
            const uint32_t bits = Read32(obj + 0x150), hash = Read32(obj + 0x16C), hsize = Read32(obj + 0x170);
            // A block the map owns: a live GMalloc block at least this large. The whole header must look like a map,
            // else the object stays out (its header comes back with the image, as before).
            auto block = [](uint32_t p, uint32_t need, uint32_t* size) {
                uint32_t bi[2] = {0, 0};
                if (!p || (p & 3) || !LiveBlockInfo(reinterpret_cast<const void*>(p), bi) || bi[1] < need || !Writable(p, need)) return false;
                *size = bi[1];
                return true;
            };
            uint32_t es = 0, fs = 0, bs = 0, hs = 0;
            const char* bad = nullptr;
            if (num > max || max > 4096 || (data ? !block(data, max * 28, &es) : max != 0)) bad = "elements";
            else if (fnum > fmax || fmax > 4096 || (fdata ? !block(fdata, fmax * 4, &fs) : fmax != 0)) bad = "free-index stack";
            else if (bits && (!block(bits, 4, &bs) || bs > 0x1000)) bad = "allocation bits";
            else if (hsize > 65536 || (hsize & (hsize - 1)) || (hash && (!hsize || !block(hash, hsize * 4, &hs)))) bad = "hash";
            if (bad) {
                if (sRejected++ < 10)
                    Logger::Info("NAMEMAP %08X (%.60s): %s not a map block (elements %08X %u/%u, stack %08X %u/%u, bits %08X, hash %08X x%u): "
                                 "not saved",
                                 obj, ObjectHooks::TypeOf(obj), bad, data, num, max, fdata, fnum, fmax, bits, hash, hsize);
                continue;
            }
            out->mapObjs.push_back(obj);
            out->mapObjs.push_back(Read32(obj + 4));
            if (data) {
                add("namemap_elems", data, max * 28, {});
                out->spans[n - 1].identity = (uint32_t)(obj + 0x138);
            }
            if (fdata) {
                add("namemap_free", fdata, fmax * 4, {});
                out->spans[n - 1].identity = (uint32_t)(obj + 0x144);
            }
            if (bits) {
                add("namemap_bits", bits, bs, {});
                out->spans[n - 1].identity = (uint32_t)(obj + 0x150);
            }
            if (hash) {
                add("namemap_hash", hash, hsize * 4, {});
                out->spans[n - 1].identity = (uint32_t)(obj + 0x16C);
            }
            if (sAccepted++ < 12 || sAccepted % 50000 == 0)
                Logger::Info("NAMEMAP %08X (%.60s) saved: elements %u/%u, stack %u/%u, bits %s, hash x%u (%u maps saved)", obj,
                             ObjectHooks::TypeOf(obj), num, max, fnum, fmax, bits ? "block" : "inline", hsize, sAccepted);
        }
    }
    Sub(8);
    // rc21: guarded reads instead of VirtualQuery through the Tier E and script phases too (until Mark(10)). Their
    // Writable checks only decide which spans are copied (LoadAB checks Writable with VirtualQuery before writing); the
    // 2026-11-09 save audit counted 209-242 VirtualQuery calls a save, ~100 of them in Tier E for 12 SkelControls, at
    // ~9 us each on the slower field PC (~2 ms of its 4.7-7 ms save). 'fastprobe 0' turns both phases back.
    skelVq += MemQuery::gVqCalls - vqAtSkel;
    Mark(7);
    if (gTierEOn) {
        BuildTierE(gameFrame);
        // Every node the fighters' components tick (USkeletalMeshComponent::TickAnimNodes 0x7627F0: tick array
        // +0x290/+0x294 of 0xC-byte entries, head +8, chain +0x78): the cached ownership scan misses nodes
        // created after it ran or owned deeper than two levels.
        static std::vector<uintptr_t> objs;
        objs.assign(gTierE.begin(), gTierE.end());
        int added = 0;
        if (gTierETickNodes) {
            std::unordered_set<uintptr_t> have(gTierE.begin(), gTierE.end());
            for (int i = 0; i < 2; ++i) {
                uint32_t id = 0;
                const uintptr_t obj = CharObj(i, &id);
                const uint32_t comp = obj ? Read32(obj + 0x10C) : 0;
                if (!comp || !Writable(comp, 0x300) || Read32(comp) != kSkelMeshVtable) continue;
                const uint32_t arr = Read32(comp + 0x290);
                const int32_t cnt = (int32_t)Read32(comp + 0x294);
                if (cnt <= 0 || cnt > 256 || !Writable(arr, cnt * 0xC)) continue;
                for (int32_t k = 0; k < cnt; ++k) {
                    uint32_t node = Read32(arr + k * 0xC + 8);
                    for (int guard = 0; node && guard < 512 && Writable(node, 0x7C); ++guard) {
                        if (have.insert(node).second && LiveUObject(node)) {
                            objs.push_back(node);
                            ++added;
                        }
                        node = Read32(node + 0x78);
                    }
                }
            }
        }
        static int addLogged = 0;
        if (added && addLogged < 6) {
            ++addLogged;
            Logger::Info("TIERE gf=%u: %d ticked anim nodes not in the ownership list added", gameFrame, added);
        }
        for (uintptr_t o : objs) {
            uint32_t info[2] = {0, 0};
            if (!Writable(o, 0x40) || !LiveBlockInfo(reinterpret_cast<const void*>(o), info)) continue;
            const uint32_t size = info[1] > 0x1000 ? 0x1000 : info[1];
            if (size <= 0x40 || !Writable(o, size)) continue;
            if (!LiveUObject(o)) continue;  // garbage-collected since the list was built
            if (gameFrame % 97 == 0) VtableKeepReport("ue3anim", o + 0x3C, size - 0x3C);
            add("ue3anim", o + 0x3C, size - 0x3C, PointerLikeOffsets(o + 0x3C, size - 0x3C));
            out->spans[n - 1].identity = Read32(o + 4);  // GObjObjects index, re-checked at load
        }
    }
    // Objects bound to the fighters' SkelControls (node +0xC0, bound by 0x0076DF10): 0x60-byte elements (vtable
    // 0xD5A5E0, ctor 0x00783E00) of a TArray, so not GMalloc blocks of their own and in no other span. The control's
    // strength is [bound+0x20] (0x007601B0); left live, a replay's first frame blended the fighters' PinLimb IK from
    // the abandoned future's strength ('animtrace' 2026-10-17: 1.0 or 0 where live ramped). Pointer-safe merge;
    // the load re-checks the vtable and the back-link to the node (+0xC).
    // The bound objects are the elements of the component's TArray +0x384 (0x60 each), and each one's strength is
    // set every frame from the component's TArray +0x39C ({float strength, float}, 8 bytes per element) by 0x0077BA40
    // (clamped, then [bound+0x20]). Neither array's data was in any span: the replay's first frame took the
    // abandoned future's weights (strength = live value at F). Both arrays are captured whole and restored only while
    // the component still uses the same blocks ('compweights', 'abtest compweights').
    for (int i = 0; i < 2; ++i) {
        uint32_t id = 0;
        const uintptr_t obj = gBindState ? CharObj(i, &id) : 0;
        SaveBindState(obj ? Read32(obj + 0x10C) : 0, out->bindRec[i]);
    }
    if (gCompWeights) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            const uintptr_t comp = obj ? Read32(obj + 0x10C) : 0;
            if (!comp || !Writable(comp + 0x384, 0x28)) continue;
            const uint32_t d384 = Read32(comp + 0x384), n384 = Read32(comp + 0x388);
            if (d384 && n384 && n384 <= 64 && !(d384 & 3) && Writable(d384, n384 * 0x60)) {
                std::vector<uint32_t> keep = PointerLikeOffsets(d384, n384 * 0x60);
                for (uint32_t e = 0; e < n384; ++e)
                    for (uint32_t o : {0x0u, 0xCu, 0x10u, 0x14u, 0x40u, 0x44u, 0x48u}) keep.push_back(e * 0x60 + o);  // links + TArray
                std::sort(keep.begin(), keep.end());
                keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
                add("compbound", d384, n384 * 0x60, std::move(keep));
                out->spans[n - 1].identity = (uint32_t)(comp + 0x384);  // the header must still name this block at load
            }
            const uint32_t d39c = Read32(comp + 0x39C), n39c = Read32(comp + 0x3A0);
            if (d39c && n39c && n39c <= 256 && !(d39c & 3) && Writable(d39c, n39c * 8)) {
                add("compweights", d39c, n39c * 8, {});
                out->spans[n - 1].identity = (uint32_t)(comp + 0x39C);
            }
        }
    }
    if (gSkelBound) {
        for (int i = 0; i < 2; ++i) {
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            const uintptr_t comp = obj ? Read32(obj + 0x10C) : 0;
            if (!comp || !Writable(comp + 0x290, 8)) continue;
            const uint32_t tab = Read32(comp + 0x290), cnt = Read32(comp + 0x294);
            if (!tab || cnt > 64 || !Writable(tab, cnt * 0xC)) continue;
            for (uint32_t k = 0; k < cnt; ++k) {
                const uint32_t node = Read32(tab + k * 0xC + 8);
                if (!node || !Writable(node + 0xC0, 4)) continue;
                const uint32_t b = Read32(node + 0xC0);
                if (!b || (b & 3) || !Writable(b, 0x60) || Read32(b) != kSkelBoundVtable || Read32(b + 0xC) != node) continue;
                std::vector<uint32_t> keep = PointerLikeOffsets(b, 0x60);
                for (uint32_t o : {0x0u, 0xCu, 0x10u, 0x14u, 0x40u, 0x44u, 0x48u}) keep.push_back(o);  // links + TArray
                std::sort(keep.begin(), keep.end());
                keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
                add("skelbound", b, 0x60, std::move(keep));
                out->spans[n - 1].identity = node;  // re-checked at load (+0xC must still name this node)
            }
        }
    }
    Mark(8);
    // MKScript context data areas: a context (current-context global [0x00E39DF8], seen at native
    // dispatch 0x0045CA08) points at its module's mutable data [+0x84, +0x88) and a variable
    // table (+0xA0). Those live in the loaded script module, outside the MK heap image, so
    // script globals (countdowns, flags) kept their abandoned-timeline values after a load
    // (native trace 2026-10-14: the decrement opcode 0x004597E0 returned values off by k).
    if (gScriptData) {
        // Contexts of every live executor (eager), plus any seen at native dispatch.
        static std::vector<uint32_t> ctxs;
        ctxs = gVmContexts;
        for (uint32_t x : RngTrace::ScriptContexts())
            if (std::find(ctxs.begin(), ctxs.end(), x) == ctxs.end()) ctxs.push_back(x);
        // The whole module allocation (base [ctx+0xAC], size [ctx+0xB0]; data [+0x84,+0x88) and
        // the variable table [+0xA0,+0xA4) live inside it). Restoring only the data range left
        // object-reference variables stale (a script deleted an object twice, 0x008D10A6).
        static std::vector<uint32_t> bases;
        bases.clear();
        // Contexts seen at dispatch accumulate for the whole fight: one whose script ended still
        // names its freed module, and restoring freed GMalloc memory by content overwrote the medium
        // heap's free tree ([inner+0x30], 0x0042AFC2 / 0x0042ACFD, 2026-10-15). So contexts come only
        // from live executors or from dispatch in the last frame (RngTrace::ScriptContexts), and a module
        // must be a live GMalloc block at least as large as the span (re-checked at load).
        static int nMod = 0, nModDead = 0, nData = 0, nDataStale = 0, saves = 0;
        for (uint32_t ctx : ctxs) {
            if (!Writable(ctx, 0xC0)) continue;
            const bool liveExec = std::find(gVmContexts.begin(), gVmContexts.end(), ctx) != gVmContexts.end();
            const uint32_t base = Read32(ctx + 0xAC), size = Read32(ctx + 0xB0);
            const uint32_t b = Read32(ctx + 0x84), e = Read32(ctx + 0x88);
            if (base && size && size <= 0x200000 && b >= base && e <= base + size && Writable(base, size)) {
                if (std::find(bases.begin(), bases.end(), base) != bases.end()) continue;
                uint32_t info[2] = {0, 0};
                if ((base & 7) || !LiveBlockInfo(reinterpret_cast<const void*>(base), info) || info[1] < size) {
                    ++nModDead;
                    continue;
                }
                bases.push_back(base);
                add("script_module", base, size, {});
                out->spans[n - 1].identity = info[1];  // GMalloc block size, re-checked at load
                ++nMod;
            } else if (base && b && e > b && e - b <= 0x100000 && Writable(b, e - b)) {
                // base == 0: the module was released and the context is dead ('ctxprobe' 2026-10-17). Its old data
                // range can lie over newer allocations, and the block-start test then passes on whatever block now
                // starts there: comprender A/B run 1 restored such a span over a block freed by 0x004D2E96 ("written
                // after free", then the medium heap crashed at 0x0042B2BC with ebx inside script_data +0x840).
                (void)liveExec;  // contexts are live executors' or dispatched in the last frame (RngTrace)
                // Only a range that starts a live GMalloc block at least as large (its exact size is
                // re-checked at load): an unchecked range was written over the medium heap's free tree.
                // The data range usually sits in one allocation with the variable table ([ctx+0xA0], below
                // the data): capture from whichever of the two starts a live block covering [start, e).
                uint32_t info[2] = {0, 0};
                const uint32_t vars = Read32(ctx + 0xA0);
                uint32_t start = 0;
                // Block starts are 8-aligned (MK chunk header {prev, size|flags}); the lookup also answers for interior
                // pointers, and a variable table 4 bytes into its allocation made a span that ran over the next chunk's
                // header (synctest31: medium free-tree crash 0x0042AD08 with esi inside script_data +0x8C4).
                if (vars && vars < b && !(vars & 7) && LiveBlockInfo(reinterpret_cast<const void*>(vars), info) && vars + info[1] >= e)
                    start = vars;
                else if (!(b & 7) && LiveBlockInfo(reinterpret_cast<const void*>(b), info) && info[1] >= e - b)
                    start = b;
                if (!start) {
                    ++nDataStale;
                    continue;
                }
                add("script_data", start, e - start, {});
                out->spans[n - 1].identity = info[1];
                ++nData;
            }
        }
        if (++saves % 1200 == 1)
            Logger::Info("SCRIPTSPANS (cumulative): modules %d, rejected (not a live block) %d; data %d, rejected (not a block start) %d",
                         nMod, nModDead, nData, nDataStale);
    }
    Mark(9);
    // MKScript executor scratch: 0x8000 bytes from GMalloc at [0x00E39FC4] (slices depth*0x2000,
    // 0x0045C700). Executors run inside a slice and copy back only under storage policy 1, so
    // the scratch may hold suspended script state (RNG trace 2026-10-14: the first diverging
    // draws after a load come from the script native random 0x008B87B0).
    if (gScriptScratch) {
        const uintptr_t scratch = Read32(0x00E39FC4);
        if (scratch && Writable(scratch, 0x8000)) add("script_scratch", scratch, 0x8000, {});
    }
    // UE3 WorldInfo clock fields only (the vt+0xBC phase is UWorld::Tick 0x799410).
    // A blanket 0x700-byte WorldInfo restore crashed the game (run23); only the fields
    // in kWorldInfoClock (found by the 'wiprobe' tick-to-tick diff) are restored.
    const uintptr_t worldInfo = WorldInfo();
    for (const ClockField& c : kWorldInfoClock)
        if (worldInfo && c.size) add(c.name, worldInfo + c.offset, c.size, {});
    out->spans.resize(n);
    out->bytes.resize(n);
    gProbeFast = false;
    Mark(10);
    if (++gProfSaves % 600 == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        auto ms = [&](int i) { return gProf[i] * 1000.0 / f.QuadPart / 600; };
        Logger::Info("SAVE PROFILE (ms/save, 600 saves): tierA %.3f master %.3f extlists %.3f xblocks %.3f ue3cells %.3f actors %.3f skel %.3f tierE %.3f script %.3f rest %.3f",
                     ms(1), ms(2), ms(3), ms(4), ms(5), ms(6), ms(7), ms(8), ms(9), ms(10));
        for (int64_t& v : gProf) v = 0;
        auto sk = [&](int i) { return skelProf[i] * 1000.0 / f.QuadPart / 600; };
        Logger::Info("SAVE SKEL (ms/save, 600 saves): fxactors %.3f movereq %.3f costdmg %.3f walk %.3f mkactors %.3f helpers %.3f "
                     "skelarrays+prop %.3f pawncomps %.3f compspan+namemaps %.3f; per save: fx %.1f mkactors %.1f helpers %.1f "
                     "propcomps %.1f; VirtualQuery calls/save: skel %.0f, all MemQuery callers %.0f",
                     sk(0), sk(1), sk(2), sk(3), sk(4), sk(5), sk(6), sk(7), sk(8), skelCounts[0] / 600.0, skelCounts[1] / 600.0,
                     skelCounts[2] / 600.0, skelCounts[3] / 600.0, skelVq / 600.0, (MemQuery::gVqCalls - saveVq) / 600.0);
        // rc25: the pointer scan's committed-region test (saves and the loads' live scans together, per save): answers from
        // the 1024-range cache of committed regions, from the per-scan cache, and VirtualQuery calls by answer.
        Logger::Info("SAVE SKEL committed test (per save, 600 saves; loads included): cache hits %.1f, scan-cache hits %.1f, VirtualQuery "
                     "%.1f (committed %.1f, other %.1f); %d ranges cached",
                     gCommittedHits / 600.0, gScanHits / 600.0, (gVqCommitted + gVqOther) / 600.0, gVqCommitted / 600.0, gVqOther / 600.0,
                     gCommittedN);
        gCommittedHits = gScanHits = gVqCommitted = gVqOther = 0;
        for (int64_t& v : skelProf) v = 0;
        for (uint32_t& v : skelCounts) v = 0;
        skelVq = 0;
        saveVq = MemQuery::gVqCalls;
        Logger::Info("SAVE spans of already-freed blocks not captured (cumulative): %u", gPendingSkips);
    }
    return true;
}

uintptr_t WorldInfoActor() { return WorldInfo(); }

int HashAB(uint32_t* perSpan, const char** names, int maxSpans, uint32_t* combined) {
    StateAB tmp;
    char why[160];
    const bool ok = SaveAB(&tmp, 0, why, sizeof why);
    gStates.erase(&tmp);  // stack temporary: must not stay registered (dangling -> crash in WriteOwnRanges)
    if (!ok) return -1;
    uint32_t all = 0;
    const int n = (int)tmp.spans.size();
    for (int i = 0; i < n; ++i) {
        std::vector<uint8_t>& b = tmp.bytes[i];
        if (!strcmp(tmp.spans[i].name, "gameinfo_all")) memset(b.data() + 0x6C, 0, 4);  // HostFrame
        for (uint32_t o : tmp.spans[i].keepOffsets) memset(b.data() + o, 0, 4);        // live pointers
        if (!strcmp(tmp.spans[i].name, "rngB")) memset(b.data(), 0, 4);  // also drawn by render/FX outside the sim
        if (!strncmp(tmp.spans[i].name, "actor", 5)) MaskActorRenderFields(b.data());
        const uint32_t h = StateHash::XXH32(b.data(), b.size(), 0);
        if (i < maxSpans) {
            perSpan[i] = h;
            names[i] = tmp.spans[i].name;
        }
        all = StateHash::XXH32(&h, 4, all);
    }
    *combined = all;
    return n;
}

// GMalloc small-block allocator (static object 0x00E0AC58, reached from GMalloc Malloc via
// 0x0042C9D9): bin count +0x7CC, bins at +0x498 stride 0x10, each an 8-byte lock-free list
// head {first free block, tag}; a free block's first dword is the next link (pop: 0x00714560).
// A free block lying inside a span means the span would write into freed memory (the
// 0x007145BD crash: free-list link overwritten). Read-only and racy with other threads; it
// is a diagnostic.
const char* CoveredStatic(uintptr_t addr, uint32_t len) {
    for (const Raw& r : TierA())
        if (addr >= r.base && addr + len <= r.base + r.size) return r.name;
    return nullptr;
}

// UE3 reflection: which dwords of a class's instances are plain data (never pointers). The pointer-safe merge guesses
// pointer vs data from the value; a float in the heap's address range that happened to start an object was kept live
// (fighter Location, PinLimb EffectorLocation/JointTargetLocation). Float/Int/Bool/Byte/Name properties and structs of
// floats/ints (Vector, Rotator, ...) are data by declaration. Layout (runtime, 2026-10-17): UStruct SuperStruct +0x38,
// PropertiesSize +0x40, Children +0x58; UProperty Next +0x3C, ArrayDim +0x44, ElementSize +0x48, Offset +0x58,
// UStructProperty Struct +0x78. Unknown/native fields stay with the heuristic.
bool gReflectData = true;
uint32_t gReflectRestored = 0, gReflectLoggedAt = 0;
// LoadAB's pointer-safe merge (rc25): 0 the whole-span merge, 1 diff-first (default), 2 diff-first checked against the
// whole-span merge on every span (DIFFMERGE MISMATCH lines; the whole-span result is used on a mismatch).
int gDiffMerge = 1;
// Since the last LOADAB PROFILE line: merged spans by diff, those with no differing dword, differing dwords, live origins
// classified, spans merged whole (fallback), mismatches in 'diffmerge 2'.
uint32_t gDiffSpans = 0, gDiffUnchanged = 0, gDiffDwords = 0, gDiffOrigins = 0, gDiffFull = 0, gDiffMismatch = 0;
const std::vector<uint8_t>& ReflectDataMask(uint32_t cls) {
    static std::unordered_map<uint32_t, std::vector<uint8_t>> cache;
    auto it = cache.find(cls);
    if (it != cache.end()) return it->second;
    std::vector<uint8_t>& m = cache[cls];
    uint32_t size = 0;
    if (!cls || !CrashLog::SafeRead32(cls + 0x40, &size) || size > 0x2000) return m;
    m.assign(size / 4 + 1, 0);
    static const char* kDataProps[] = {"FloatProperty", "IntProperty", "BoolProperty", "ByteProperty", "NameProperty"};
    static const char* kDataStructs[] = {"Vector", "Rotator", "Quat", "Vector2D", "Color", "LinearColor", "Plane", "Matrix", "Box", "Sphere",
                                         "IntPoint", "TwoVectors", "Vector4", "BoxSphereBounds",
                                         "LightingChannelContainer", "RBCollisionChannelContainer", "BeastPrimPropertiesContainer"};  // bitfields
    static std::unordered_set<std::string> sOtherStructs;  // struct types met but not classified (logged once each)
    auto baseName = [](uint32_t obj, char* out, size_t n) {
        Ue3Names::NameOf(obj, out, n);
        if (char* u = strrchr(out, '_')) *u = 0;  // "FloatProperty_0" -> "FloatProperty"
    };
    int marked = 0;
    for (uint32_t c = cls, depth = 0; c && depth < 12; ++depth) {
        uint32_t p = 0, next = 0;
        CrashLog::SafeRead32(c + 0x58, &p);
        // Diagnostic (2026-11-04, complivedata): the walk stops at the first non-property field. A class's field list
        // can also hold functions, structs, enums, consts and states; count the data dwords past such a stop (not
        // marked) to learn whether declared data is being left to the pointer heuristic.
        const char* stoppedAt = nullptr;
        char stopName[64] = "";
        int lost = 0, skippedFields = 0;
        for (int guard = 0; p && guard < 1024; ++guard, p = next) {
            uint32_t pc = 0, off = 0, dim = 0, elem = 0;
            if (!CrashLog::SafeRead32(p + 0x30, &pc) || !CrashLog::SafeRead32(p + 0x3C, &next))
                break;
            char cn[64];
            baseName(pc, cn, sizeof cn);
            if (!strstr(cn, "Property")) {
                static const char* kFieldKinds[] = {"Function", "ScriptStruct", "Struct", "Enum", "Const", "State"};
                bool field = false;
                for (const char* k : kFieldKinds) field |= !strcmp(cn, k);
                if (!stoppedAt) {
                    strncpy(stopName, cn, sizeof stopName - 1);
                    stoppedAt = stopName;
                }
                if (!field) break;  // not a field list (layout mismatch): stop, keep what is known
                ++skippedFields;
                continue;
            }
            if (!CrashLog::SafeRead32(p + 0x58, &off) || !CrashLog::SafeRead32(p + 0x44, &dim) || !CrashLog::SafeRead32(p + 0x48, &elem))
                break;
            bool data = false;
            for (const char* d : kDataProps) data |= !strcmp(cn, d);
            if (!data && !strcmp(cn, "StructProperty")) {
                uint32_t st = 0;
                char sn[64] = "";
                if (CrashLog::SafeRead32(p + 0x78, &st) && st) baseName(st, sn, sizeof sn);
                for (const char* d : kDataStructs) data |= !strcmp(sn, d);
                if (!data && *sn && sOtherStructs.insert(sn).second) {
                    char pn[64];
                    Ue3Names::NameOf(p, pn, sizeof pn);
                    Logger::Info("REFLECT   struct %s (property %s +0x%X, %u bytes) left to the heuristic", sn, pn, off, dim * elem);
                }
            }
            if (!data || dim == 0 || dim > 256 || elem == 0 || elem > 0x100) continue;
            const uint32_t end = off + dim * elem;
            if (end > size) continue;
            for (uint32_t o = off & ~3u; o < end; o += 4) {
                if (o / 4 >= m.size()) continue;
                if (stoppedAt) {  // the original walk had stopped: count only
                    if (!m[o / 4]) ++lost;
                } else {
                    m[o / 4] = 1, ++marked;
                }
            }
        }
        if (stoppedAt) {
            char own[64];
            Ue3Names::NameOf(c, own, sizeof own);
            Logger::Info("REFLECT   %s: field list stopped at a %s; past it %d more field(s) and %d declared data dword(s) not marked",
                         own, stoppedAt, skippedFields, lost);
        }
        uint32_t super = 0;
        if (!CrashLog::SafeRead32(c + 0x38, &super)) break;
        c = super;
    }
    char cn[64];
    Ue3Names::NameOf(cls, cn, sizeof cn);
    Logger::Info("REFLECT %s %08X: size %X, %d data dwords", cn, cls, size, marked);
    return m;
}

// Fighter component +0x3A8 list after a load. The fight tick walks it (0x0077D535): an element with +8 == 0 and both
// weights (+0x20, +0x24) above the threshold has its node read from +0xC and used without a null check
// ([node+0xA0], 0x0077D56A). An X-ray's IK track instance (CinemaTrackInstIK, its element at +0x48) is unbound
// (node 0) and garbage-collected at the X-ray end; a load to a frame before that collection puts the saved list back
// naming it while the cinema restore leaves the collected instance out (batch 21, xray3a ns100: both games crashed at
// resim 100474 after loading 100473). Removed here, as the game's own unbind (0x00767060) removes it: an entry that
// is unreadable, inside a cinematic object collected since S, or unbound with +8 == 0 (the walk's crash condition).
bool gCompListScrub = true;
uint32_t gCompListScrubbed = 0;
static uint32_t gCompListScrubLogged = 0;
int ScrubCompLists(uint32_t S, const std::vector<std::pair<uintptr_t, uint32_t>>& gone) {
    if (!gCompListScrub) return 0;
    int removed = 0;
    for (int i = 0; i < 2; ++i) {
        uint32_t id = 0;
        const uintptr_t obj = CharObj(i, &id);
        const uintptr_t comp = obj ? Read32(obj + 0x10C) : 0;
        if (!comp || !Writable(comp, 0x3B0) || Read32(comp) != kSkelMeshVtable) continue;
        const uint32_t ld = Read32(comp + 0x3A8), ln = Read32(comp + 0x3AC);
        if (!ld || !ln || ln > 256 || !Writable(ld, ln * 4)) continue;
        uint32_t* list = reinterpret_cast<uint32_t*>(ld);
        uint32_t kept = 0;
        for (uint32_t k = 0; k < ln; ++k) {
            const uint32_t e = list[k];
            uint32_t in = 0;
            for (const auto& g : gone)
                if (e >= g.first && e < g.first + g.second) in = (uint32_t)g.first;
            const bool readable = e && Writable(e, 0x28);
            const char* why = !readable ? "unreadable"
                              : in ? "inside a cinematic object collected since S"
                              : (Read32(e + 8) == 0 && Read32(e + 0xC) == 0) ? "unbound (node 0)" : nullptr;
            if (!why) {
                list[kept++] = e;
                continue;
            }
            ++removed;
            if (gCompListScrubLogged < 30) {
                ++gCompListScrubLogged;
                char cn[64] = "?";
                if (in) Ue3Names::NameOf(in, cn, sizeof cn);
                Logger::Info("COMPLIST load S=%u: P%d comp %08X +0x3A8[%u] = %08X removed: %s%s%s (+8 %08X, +0xC %08X)", S, i + 1,
                             (uint32_t)comp, k, e, why, in ? ", " : "", in ? cn : "", readable ? Read32(e + 8) : 0,
                             readable ? Read32(e + 0xC) : 0);
            }
        }
        if (kept != ln) *reinterpret_cast<uint32_t*>(comp + 0x3AC) = kept;
    }
    gCompListScrubbed += removed;
    return removed;
}

// Fighter SkelControl binding state, saved and restored as one unit (2026-10-17). The leg-IK strength comes from
// bound elements (component TArray +0x384, 0x60 each, vtable 0xD5A5E0) whose strength 0x0077BA40 sets every frame
// from the weights TArray +0x39C, binding an element (0x0076DF10) once its weight rises: node +0xC0 = element,
// element +0xC = node / +0x10 = component, helper [element+0x14] +0x20 = 0 / +0x24 = comp+0x658, and the element is
// appended to the component's +0x3A8 list. Unbinding (0x0076DF10 path, 0x00767060 removes from the list, vt+8 resets
// the element's own fields) is local to these objects. Restoring part of it (compweights) left contradictory links
// and crashed in the bind; this restores all of it, or nothing when any block, count or node changed since the save.
bool gPinFloats = true;  // PinLimb strength and vector fields always restored by value ('abtest pinfloats')
uint32_t gPinFloatsRestored = 0;
bool gCompFloats = true;
uint32_t gCompFloatsRestored = 0;
bool gCompLiveData = true;  // fighter-component kept dwords whose live value is data restored by value ('abtest complivedata')
uint32_t gCompLiveDataRestored = 0;
bool gListRescue = true;  // restore a fighter comp's freed +0x3A8 list block from the quarantine ('abtest listrescue')
uint32_t gListRescued = 0;
bool gBindState = true;
bool gBindStateLoad = true;
bool gListRebase = true;
bool gFxActors = true, gFxActorsLoad = true;  // fighters' attached actors ('abtest fxactors')
bool gMkActors = true, gMkActorsLoad = true;  // other MK objects' actors ('abtest mkactors')
bool gMoveReqs = true, gMoveReqLoad = true;  // fighters' move-request entries ('abtest movereq')
bool gCostDmg = true, gCostDmgLoad = true;  // fighters' costume damage pieces ('abtest costdmg')
uint32_t gMkActorSaved = 0, gMkActorRestored = 0;
bool gMkBases = true;  // MK actors' Base actors in no other span ('abtest mkbases')
uint32_t gMkBaseSaved = 0;
bool gMkHelpers = true, gMkHelpersLoad = true;  // MK objects' +0x1D8 helper blocks ('abtest mkhelper')
uint32_t gMkHelperSaved = 0, gMkHelperRestored = 0;
bool gMkHelperRevive = true;
uint32_t gMkHelperRevived = 0;
namespace {
// What the last load did with each MK helper span, for VMTRACE: soak 13104 S=3405 (2026-11-04) had a tick-gate
// [+0x1D8]+0xC 0 in the original and 1 in the replay with the mkhelper restore on (the 11804 case it should cover).
struct HelperOutcome {
    uint32_t owner, base, cell, savedC, liveC;
    uint8_t why;  // 0 restored, 1 'abtest mkhelper' load off, 2 not writable, 3 vtable changed, 4 owner's +0x1D8 moved, 5 block
};
HelperOutcome gHelperLast[32];
uint32_t gHelperLastN = 0, gHelperLastSpans = 0;
std::vector<uint32_t> gHelperRestored;  // every helper the last load restored
}  // namespace
const std::vector<uint32_t>& RestoredHelpers() { return gHelperRestored; }
const char* NameRestored(uint32_t addr, uint32_t* base);
void DescribeHelper(uint32_t owner, uint32_t helper, char* out, size_t n) {
    static const char* const kWhy[] = {"restored", "skipped (load off)", "skipped (not writable)", "skipped (vtable changed)",
                                       "skipped (owner +0x1D8 points elsewhere)", "skipped (block not live)"};
    for (uint32_t k = 0; k < gHelperLastN; ++k) {
        const HelperOutcome& o = gHelperLast[k];
        if (o.owner != owner && o.base != helper) continue;
        snprintf(out, n, "last load: helper %08X of owner %08X %s, owner +0x1D8 %08X, +0xC saved %08X live-before-load %08X", o.base,
                 o.owner, kWhy[o.why < 6 ? o.why : 0], o.cell, o.savedC, o.liveC);
        return;
    }
    uint32_t base = 0;
    const char* in = helper ? NameRestored(helper, &base) : nullptr;
    snprintf(out, n, "last load had no MK helper span for it (%u helper spans); helper %s%s; owner born gf %u, type %.60s", gHelperLastSpans,
             in ? "inside span " : "in no span", in ? in : "", ObjectHooks::BirthGf(owner), ObjectHooks::TypeOf(owner));
}
bool gPadFullLoad = true;  // restore the whole 0x60C AI/remote pad, incl. the ring index at +0x5F8 ('abtest padfull')
// Hardware pad slot +8 bits 0x1 (a device is attached: the poll 0x00424A60 calls the device in the table 0x00E0E1F0 +
// slot*0x17C, +0xCC) and 0x20 (its kind) belong to the device manager: set only by the attach 0x00422FC0 (with the
// device record), cleared only by the release 0x004219D8 (with the device), which also runs in the re-detection 300 ms
// after a WM_DEVICECHANGE (flag 0x00DE9934, 0x00424A89 -> 0x00422E10; Input.cpp, HoldDeviceChange). The hwpads span
// restored them with the input state, so a load after a release brought back "attached" over a released device: traced
// soak 13120 (2026-11-05 01:55), null read at 0x00424B1E (GetDeviceState through a null device), game closed. Kept
// live; the rest of the record is restored.
bool gPadHwLive = true;
uint32_t gPadHwKept = 0;
namespace {
constexpr uint32_t kPadDeviceBits = 0x21;
constexpr int kHwPadSlots = 6;
void KeepPadDeviceBits(uintptr_t base, const uint32_t* live, uint32_t S) {
    for (int p = 0; p < kHwPadSlots; ++p) {
        uint32_t& f = *reinterpret_cast<uint32_t*>(base + p * kPadObjSize + 8);
        if (!((f ^ live[p]) & kPadDeviceBits)) continue;
        if (gPadHwKept++ < 20 || gPadHwKept % 100 == 0)
            Logger::Info("PADHW S=%u: pad slot %d device bits saved %X, live %X: kept live (%u so far)", S, p, f & kPadDeviceBits,
                         live[p] & kPadDeviceBits, gPadHwKept);
        f = (f & ~kPadDeviceBits) | (live[p] & kPadDeviceBits);
    }
}
}  // namespace
uint32_t gFxActorSaved = 0, gFxActorRestored = 0;  // write the saved list into a reallocated live +0x3A8 block ('abtest listrebase')
uint32_t gBindApplied = 0, gBindSkipped = 0;
namespace {
constexpr uint32_t kBindElemVt = 0x00D5A5E0;
bool ElemOffsetRestored(uint32_t o) { return o != 0x0 && o != 0x14 && o != 0x40 && o != 0x44 && o != 0x48; }  // vt, helper, TArray
}  // namespace
void SaveBindState(uint32_t comp, std::vector<uint32_t>& r) {
    r.clear();
    if (!comp || !Writable(comp + 0x290, 0x130)) return;
    const uint32_t d384 = Read32(comp + 0x384), n384 = Read32(comp + 0x388), d39c = Read32(comp + 0x39C), n39c = Read32(comp + 0x3A0);
    const uint32_t d3a8 = Read32(comp + 0x3A8), n3a8 = Read32(comp + 0x3AC), tab = Read32(comp + 0x290), cnt = Read32(comp + 0x294);
    if (n384 > 16 || n39c > 64 || n3a8 > 16 || cnt > 32) return;
    if ((n384 && (!d384 || !Writable(d384, n384 * 0x60))) || (n39c && (!d39c || !Writable(d39c, n39c * 8))) ||
        (n3a8 && (!d3a8 || !Writable(d3a8, n3a8 * 4))) || (cnt && (!tab || !Writable(tab, cnt * 0xC))))
        return;
    r = {comp, d384, n384, d39c, n39c, d3a8, n3a8, tab, cnt};
    for (uint32_t k = 0; k < cnt; ++k) {
        const uint32_t node = Read32(tab + k * 0xC + 8);
        r.push_back(node);
        r.push_back(node && Writable(node + 0xC0, 4) ? Read32(node + 0xC0) : 0);
    }
    for (uint32_t e = 0; e < n384; ++e) {
        const uint32_t el = d384 + e * 0x60;
        for (uint32_t o = 0; o < 0x60; o += 4) r.push_back(Read32(el + o));
        const uint32_t h = Read32(el + 0x14);
        const bool hv = h && Writable(h, 0x28);
        r.push_back(hv ? Read32(h + 0x20) : 0);
        r.push_back(hv ? Read32(h + 0x24) : 0);
    }
    for (uint32_t w = 0; w < n39c * 2; ++w) r.push_back(Read32(d39c + w * 4));
    for (uint32_t l = 0; l < n3a8; ++l) r.push_back(Read32(d3a8 + l * 4));
    // The limb pins and their requests (bindpins): see RestoreBindState. Invalid arrays are saved as absent (0, 0).
    uint32_t d378 = Read32(comp + 0x378), n378 = Read32(comp + 0x37C), d390 = Read32(comp + 0x390), n390 = Read32(comp + 0x394);
    if (n378 > 16 || (n378 && (!d378 || !Writable(d378, n378 * 0x1C)))) d378 = n378 = 0;
    if (n390 > 16 || (n390 && (!d390 || !Writable(d390, n390 * 0x48)))) d390 = n390 = 0;
    r.push_back(d378);
    r.push_back(n378);
    r.push_back(d390);
    r.push_back(n390);
    for (uint32_t q = 0; q < n378 * 7; ++q) r.push_back(Read32(d378 + q * 4));
    for (uint32_t p = 0; p < n390; ++p) {
        const uint32_t pin = d390 + p * 0x48;
        for (uint32_t o = 0; o < 0x48; o += 4) r.push_back(Read32(pin + o));
        const uint32_t h = Read32(pin + 0x14);
        const bool hv = h && Writable(h, 0x28);
        r.push_back(hv ? Read32(h + 0x20) : 0);
        r.push_back(hv ? Read32(h + 0x24) : 0);
    }
}
// The leg-IK limb pins (traced soaks 3-9, 2026-11-05): a SkelControlPinLimb node's +0xC0 points not at a +0x384
// element but at a pin of the component's TArray +0x390 (0x48 each: +0xC node, +0x14 helper, +0x20 strength, +0x28/+0x34
// targets, +0x40 flags), which 0x00779E10 feeds from the request TArray +0x378 (0x1C each: offsets, +0x18 strength,
// consumed): a request above the threshold sets the pin's strength and targets (0x0077A156), one below unbinds it (node
// +0x40 = 0, pin +0xC/+0x10 cleared). 0x007601B0 copies the pin's strength into node +0x40 before each skeleton update.
// Neither array was in any snapshot, so after a load inside a leg-IK fade node +0x40 took the abandoned future's
// strength on the first replayed frame (13119 S=3309, a real desync; IKDIFF: elements and weights right, +0x40 the
// future's; soak 7 13128: nodes bound to 0x48-apart objects live, unbound in the replay). Now part of the bind state:
// both arrays restored with the rest when their blocks and counts are unchanged, node +0xC0 may name a pin, and a pin
// in the +0x3A8 list is accepted ('abtest bindpins').
bool gBindPins = true;
bool RestoreBindState(uint32_t liveComp, const std::vector<uint32_t>& r) {
    // Each refusal logs its reason (first 3 per reason) with the values it compared.
    static int sLogged[16] = {};
    auto fail = [](int why, uint32_t a, uint32_t b) {
        if (why >= 0 && why < 16 && sLogged[why]++ < 3) Logger::Info("BINDSTATE skip %d: %08X vs %08X", why, a, b);
        if (why >= 0 && why < 16 && sLogged[why] % 100 == 0) Logger::Info("BINDSTATE skip reason %d: %d times", why, sLogged[why]);
        return false;
    };
    if (r.size() < 9) return fail(0, (uint32_t)r.size(), 9);
    const uint32_t comp = r[0], d384 = r[1], n384 = r[2], d39c = r[3], n39c = r[4], d3a8 = r[5], n3a8 = r[6], tab = r[7], cnt = r[8];
    if (comp != liveComp) return fail(1, comp, liveComp);
    const size_t pinHdr = 9 + cnt * 2 + n384 * 26 + n39c * 2 + n3a8;
    if (r.size() < pinHdr + 4) return fail(2, (uint32_t)r.size(), 0);
    const uint32_t d378 = r[pinHdr], n378 = r[pinHdr + 1], d390 = r[pinHdr + 2], n390 = r[pinHdr + 3];
    if (r.size() != pinHdr + 4 + n378 * 7 + n390 * 20) return fail(2, (uint32_t)r.size(), 1);
    const bool pins = gBindPins && (n378 || n390);
    if (pins) {
        if (Read32(comp + 0x378) != d378 || Read32(comp + 0x37C) != n378) return fail(12, d378 ^ Read32(comp + 0x378), n378 * 0x10000 + Read32(comp + 0x37C));
        if (Read32(comp + 0x390) != d390 || Read32(comp + 0x394) != n390) return fail(13, d390 ^ Read32(comp + 0x390), n390 * 0x10000 + Read32(comp + 0x394));
        if ((n378 && !Writable(d378, n378 * 0x1C)) || (n390 && !Writable(d390, n390 * 0x48))) return fail(13, d378, d390);
        size_t j = pinHdr + 4 + n378 * 7;
        for (uint32_t p = 0; p < n390; ++p, j += 20) {
            const uint32_t pin = d390 + p * 0x48;
            if (!r[j] || Read32(pin) != r[j]) return fail(14, Read32(pin), r[j]);  // same class
            if (Read32(pin + 0x14) != r[j + 5]) return fail(15, Read32(pin + 0x14), r[j + 5]);  // same helper
            if (r[j + 5] && !Writable(r[j + 5], 0x28)) return fail(15, r[j + 5], 0);
        }
    }
    auto isPin = [&](uint32_t v) { return pins && n390 && v >= d390 && v < d390 + n390 * 0x48 && !((v - d390) % 0x48); };
    // Same blocks and counts (the arrays may hold more capacity, never a different block).
    if (Read32(comp + 0x384) != d384 || Read32(comp + 0x388) != n384) return fail(3, d384 ^ Read32(comp + 0x384), n384 * 0x10000 + Read32(comp + 0x388));
    if (Read32(comp + 0x39C) != d39c || Read32(comp + 0x3A0) != n39c) return fail(4, d39c ^ Read32(comp + 0x39C), n39c * 0x10000 + Read32(comp + 0x3A0));
    if (Read32(comp + 0x290) != tab || Read32(comp + 0x294) != cnt) return fail(5, tab, cnt);
    // The list may have been reallocated since the save (a different block with enough capacity): the saved entries
    // are written into the live block (order/2026-10-17: +0x3A8 alone was the most frequent first-frame difference).
    const uint32_t liveList = Read32(comp + 0x3A8);
    if (n3a8 && (!liveList || Read32(comp + 0x3B0) < n3a8 || !Writable(liveList, n3a8 * 4) || (!gListRebase && liveList != d3a8)))
        return fail(6, d3a8 ^ liveList, n3a8 * 0x10000 + Read32(comp + 0x3B0));
    size_t i = 9;
    for (uint32_t k = 0; k < cnt; ++k, i += 2) {
        const uint32_t node = r[i], bound = r[i + 1];
        if (Read32(tab + k * 0xC + 8) != node || (node && !Writable(node + 0xC0, 4))) return fail(7, node, Read32(tab + k * 0xC + 8));
        (void)bound;  // a node whose +0xC0 is not a binding slot (a float, e.g. 0x3E5CFBBD) is skipped at write time
    }
    const size_t elemAt = i;
    for (uint32_t e = 0; e < n384; ++e, i += 26) {
        const uint32_t el = d384 + e * 0x60;
        if (Read32(el) != kBindElemVt || Read32(el) != r[i]) return fail(9, Read32(el), r[i]);
        if (Read32(el + 0x14) != r[i + 5]) return fail(10, Read32(el + 0x14), r[i + 5]);
        const uint32_t h = r[i + 5];
        if (h && !Writable(h, 0x28)) return fail(10, h, 0);
    }
    for (uint32_t l = 0; l < n3a8; ++l) {
        const uint32_t el = r[i + n39c * 2 + l];
        if (el >= d384 && el < d384 + n384 * 0x60 && !((el - d384) % 0x60)) continue;
        if (isPin(el)) continue;  // a limb pin: restored with the rest below
        // Not one of the +0x384 elements (some characters bind other objects too, st2 2026-10-18: reason 11 on
        // every load of a matchup): accepted only while the same object is still in the live list (still bound).
        bool stillBound = false;
        const uint32_t liveN = Read32(comp + 0x3AC);
        for (uint32_t m = 0; liveList && m < liveN && m < 16 && Writable(liveList + m * 4, 4); ++m) stillBound |= Read32(liveList + m * 4) == el;
        if (!stillBound) return fail(11, el, d384);
    }
    // All checks passed: write the whole state.
    i = 9;
    auto isElem = [&](uint32_t v) { return v >= d384 && v < d384 + n384 * 0x60 && !((v - d384) % 0x60); };
    for (uint32_t k = 0; k < cnt; ++k, i += 2) {
        if (!r[i]) continue;
        uint32_t& slot = *reinterpret_cast<uint32_t*>(r[i] + 0xC0);
        // Binding slot only: saved and live values are both null or one of the elements (other node classes keep
        // data at +0xC0, restored with their own span).
        if ((!r[i + 1] || isElem(r[i + 1]) || isPin(r[i + 1])) && (!slot || isElem(slot) || isPin(slot))) slot = r[i + 1];
    }
    i = elemAt;
    for (uint32_t e = 0; e < n384; ++e, i += 26) {
        const uint32_t el = d384 + e * 0x60;
        for (uint32_t o = 0; o < 0x60; o += 4)
            if (ElemOffsetRestored(o)) *reinterpret_cast<uint32_t*>(el + o) = r[i + o / 4];
        if (const uint32_t h = r[i + 5]) {
            *reinterpret_cast<uint32_t*>(h + 0x20) = r[i + 24];
            *reinterpret_cast<uint32_t*>(h + 0x24) = r[i + 25];
        }
    }
    for (uint32_t w = 0; w < n39c * 2; ++w, ++i) *reinterpret_cast<uint32_t*>(d39c + w * 4) = r[i];
    for (uint32_t l = 0; l < n3a8; ++l, ++i) *reinterpret_cast<uint32_t*>(liveList + l * 4) = r[i];  // live block (same or reallocated)
    *reinterpret_cast<uint32_t*>(comp + 0x3AC) = n3a8;
    if (pins) {
        i = pinHdr + 4;
        for (uint32_t q = 0; q < n378 * 7; ++q, ++i) *reinterpret_cast<uint32_t*>(d378 + q * 4) = r[i];
        for (uint32_t p = 0; p < n390; ++p, i += 20) {
            const uint32_t pin = d390 + p * 0x48;
            for (uint32_t o = 4; o < 0x48; o += 4)  // all but the vtable; the helper pointer was checked equal
                if (o != 0x14) *reinterpret_cast<uint32_t*>(pin + o) = r[i + o / 4];
            if (const uint32_t h = r[i + 5]) {
                *reinterpret_cast<uint32_t*>(h + 0x20) = r[i + 18];
                *reinterpret_cast<uint32_t*>(h + 0x24) = r[i + 19];
            }
        }
        static uint32_t sPins = 0;
        if (n390 && (++sPins == 1 || sPins % 1000 == 0))
            Logger::Info("BINDPINS: %u bind-state restores with limb pins (comp %08X: %u requests, %u pins)", sPins, comp, n378, n390);
    }
    return true;
}

// Where do script contexts' data ranges live ('ctxprobe')? SCRIPTSPANS showed every data-only context
// rejected as "not a block start" (ab2 2026-10-17: data 0 of 5305), so their globals are never restored.
void CtxProbe() {
    std::vector<uint32_t> ctxs = gVmContexts;
    for (uint32_t x : RngTrace::ScriptContexts())
        if (std::find(ctxs.begin(), ctxs.end(), x) == ctxs.end()) ctxs.push_back(x);
    int shown = 0;
    for (uint32_t ctx : ctxs) {
        if (!Writable(ctx, 0xC0) || ++shown > 40) continue;
        const uint32_t base = Read32(ctx + 0xAC), size = Read32(ctx + 0xB0), b = Read32(ctx + 0x84), e = Read32(ctx + 0x88);
        const uint32_t vars = Read32(ctx + 0xA0), varsEnd = Read32(ctx + 0xA4);
        uint32_t ib[2] = {0, 0}, iv[2] = {0, 0}, hb = 0, hv = 0;
        // A context whose module was released still names its freed range (module 0): guarded reads only.
        const bool okb = b && CrashLog::SafeRead32(b - 4, &hb) && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(b), ib);
        const bool okv = vars && CrashLog::SafeRead32(vars - 4, &hv) && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(vars), iv);
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<const void*>(b), &mbi, sizeof mbi);
        Logger::Info("CTXPROBE ctx %08X module %08X+%X data [%08X,%08X) %X vars [%08X,%08X) | data block %d {%08X %X} hdr %08X | vars block %d {%08X %X} hdr %08X | alloc %08X+%X type %X",
                     ctx, base, size, b, e, e - b, vars, varsEnd, okb, ib[0], ib[1], hb, okv, iv[0], iv[1], hv,(unsigned)(uintptr_t)mbi.AllocationBase, (unsigned)mbi.RegionSize, (unsigned)mbi.Type);
    }
    Logger::Info("CTXPROBE %u contexts (%u live executors)", (unsigned)ctxs.size(), (unsigned)gVmContexts.size());
}

// Per-frame watch of one small-block bin (soak 5-9: every 0x007145BD crash was bin 29, blocks of
// 625..704 bytes, with a position-like float written over a free block's next link). On the
// first broken link, log the block and every dword in the process that still points at it,
// identified as UE3 object fields where possible. Once per session.
bool WatchBin(uint32_t bin, uint32_t gf) {
    static bool fired = false;
    if (fired) return false;
    constexpr uintptr_t kSba = 0x00E0AC58;
    if (bin >= Read32(kSba + 0x7CC)) return false;
    auto readable = [](uintptr_t a) {
        MEMORY_BASIC_INFORMATION mbi;
        return a >= 0x10000 && !(a & 3) && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) &&
               mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE) &&
               a + 4 <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    };
    uintptr_t node = Read32(kSba + 0x498 + 0x10 * bin), prev = 0;
    for (int d = 0; node && d < 4096; ++d) {
        if (!readable(node)) {
            if (!prev) return false;  // head itself: likely a racing pop
            // Other threads pop this lock-free list concurrently: a node popped between reading the
            // link and following it shows its new owner's data. Confirm: the same node must still
            // be in the list, with the same bad link, after a short wait.
            Sleep(1);
            bool confirmed = false;
            for (uintptr_t n2 = Read32(kSba + 0x498 + 0x10 * bin), g2 = 0; n2 && g2 < 4096 && readable(n2); n2 = Read32(n2), ++g2)
                if (n2 == prev) {
                    confirmed = Read32(prev) == node;
                    break;
                }
            if (!confirmed) return false;
            fired = true;
            char line[200];
            int n = 0;
            for (int i = 0; i < 12; ++i) n = AppendF(line, sizeof line, n, " %08X", Read32(prev + 4 * i));
            Logger::Info("BINWATCH gf=%08X bin %u: free block %08X (depth %d) has garbage link %08X; contents:%s", gf, bin,
                         (unsigned)prev, d - 1, (unsigned)node, line);
            uint32_t fg = 0, rg = 0, caller = 0;
            if (FreeQuarantine::RecentlyReleased(reinterpret_cast<void*>(prev), 1, &fg, &rg, &caller))
                Logger::Info("BINWATCH   block was freed by the game at gf=%u (free called from %08X) and released by the quarantine at gf=%u",
                             fg, caller, rg);
            else
                Logger::Info("BINWATCH   block was not released by the quarantine recently (freed directly)");
            // Who still points at it?
            int holders = 0;
            MEMORY_BASIC_INFORMATION mbi;
            for (uintptr_t a = 0x10000; a < 0xFFFE0000 && holders < 24;) {
                if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
                const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
                    !(mbi.Protect & PAGE_GUARD)) {
                    for (uintptr_t p = rb; p + 4 <= re && holders < 24; p += 4) {
                        const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
                        if (v < prev || v >= prev + 704 || InOwnBuffers(p)) continue;
                        char who[160];
                        Ue3Names::Describe((uint32_t)p, who, sizeof who);
                        Logger::Info("BINWATCH   holder %08X -> %08X (+0x%X)  %s", (unsigned)p, v, v - (unsigned)prev, who);
                        ++holders;
                    }
                }
                a = re;
            }
            Logger::Info("BINWATCH   %d holders", holders);
            // Could a restore be the writer? Any snapshot span covering the block?
            int spans = 0;
            for (const StateAB* s : gStates)
                for (const Span& sp : s->spans)
                    if (prev + 4 > sp.base && prev < sp.base + sp.size && spans++ < 8)
                        Logger::Info("BINWATCH   covered by snapshot gf=%u span %s %08X+%X", s->gameFrame, sp.name,
                                     (unsigned)sp.base, sp.size);
            Logger::Info("BINWATCH   %d snapshot spans cover it", spans);
            Logger::Flush();
            return true;
        }
        prev = node;
        node = Read32(node);
    }
    return false;
}

int CheckFreeLists(const StateAB& st, const char* when, int maxLog) {
    constexpr uintptr_t kSba = 0x00E0AC58;
    const uint32_t bins = Read32(kSba + 0x7CC);
    if (bins == 0 || bins > 64) return -1;
    std::vector<std::pair<uintptr_t, size_t>> idx;  // (base, span index), sorted
    idx.reserve(st.spans.size());
    for (size_t i = 0; i < st.spans.size(); ++i) idx.emplace_back(st.spans[i].base, i);
    std::sort(idx.begin(), idx.end());
    uintptr_t qLo = 1, qHi = 0;  // cached readable region
    auto readable = [&](uintptr_t a) {
        if (a >= qLo && a + 4 <= qHi) return true;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD))
            return false;
        qLo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        qHi = qLo + mbi.RegionSize;
        return a + 4 <= qHi;
    };
    int hits = 0, broken = 0, walked = 0;
    for (uint32_t b = 0; b < bins; ++b) {
        uintptr_t node = Read32(kSba + 0x498 + 0x10 * b);
        for (int d = 0; node && d < 4096; ++d, ++walked) {
            if ((node & 3) || !readable(node)) {
                if (broken++ < maxLog) Logger::Info("FREELIST %s: bin %u depth %d invalid link %08X", when, b, d, (unsigned)node);
                break;
            }
            auto it = std::upper_bound(idx.begin(), idx.end(), std::make_pair(node, (size_t)-1));
            if (it != idx.begin()) {
                --it;
                const Span& s = st.spans[it->second];
                if (node < s.base + s.size) {
                    if (hits++ < maxLog)
                        Logger::Info("FREELIST %s: bin %u free block %08X inside span %s %08X+%X", when, b,
                                     (unsigned)node, s.name, (unsigned)s.base, s.size);
                }
            }
            node = Read32(node);
        }
    }
    if (hits || broken) Logger::Info("FREELIST %s: walked %d free blocks, %d inside spans, %d broken chains", when, walked, hits, broken);
    return hits + broken;
}

// Per load: what happened to each MK-object actor span (soak 12611 S=2362: a collision object's actor held the
// abandoned future's Location at the first replayed hit check, then trailed live by one frame; skipped span or moved
// after the load?). Read by the animtrace HITPOS lines.
struct MkLocRec {
    uint32_t actor;
    int status;  // 0 restored, 1 block/UObject/identity changed, 2 bDeleteMe, 3 'abtest mkactors' off, 9 skipped later
    float loc[3];
};
MkLocRec gMkLoc[512];
int gMkLocN = 0;
void NoteMkLoc(uint32_t actor, int status, const uint8_t* span) {
    MkLocRec* r = nullptr;
    for (int k = 0; k < gMkLocN && !r; ++k)
        if (gMkLoc[k].actor == actor) r = &gMkLoc[k];
    if (!r) {
        if (gMkLocN >= 512) return;
        r = &gMkLoc[gMkLocN++];
    }
    r->actor = actor;
    r->status = status;
    if (span) memcpy(r->loc, span + 0xA0, sizeof r->loc);  // span starts at +0x3C: Location +0xDC
    else r->loc[0] = r->loc[1] = r->loc[2] = 0.0f;
}
int MkActorsRestored(uint32_t* out, int max) {
    int n = 0;
    for (int k = 0; k < gMkLocN && n < max; ++k)
        if (gMkLoc[k].status == 0) out[n++] = gMkLoc[k].actor;
    return n;
}
int MkActorAtLoad(uint32_t actor, float loc[3]) {
    for (int k = 0; k < gMkLocN; ++k)
        if (gMkLoc[k].actor == actor) {
            memcpy(loc, gMkLoc[k].loc, sizeof gMkLoc[k].loc);
            return gMkLoc[k].status;
        }
    return -1;
}

const StateAB* gLastLoaded = nullptr;
int gXblockSkipped = 0;  // per load: xblocks no longer allocated (not restored)
std::map<std::string, int> gSkipNames;  // ... by span name
void SkipSpan(const char* name) {
    ++gXblockSkipped;
    ++gSkipNames[name];
}
// Crash diagnostics: which span of the last loaded snapshot contains addr.
const char* NameRestored(uint32_t addr, uint32_t* base) {
    const StateAB* s = gLastLoaded;
    if (!s || addr < 0x10000) return nullptr;
    for (const Span& sp : s->spans)
        if (addr >= sp.base && addr < sp.base + sp.size) {
            *base = (uint32_t)sp.base;
            return sp.name;
        }
    return nullptr;
}

const char* SavedDword(uint32_t addr, uint32_t* value) {
    const StateAB* s = gLastLoaded;
    if (!s || addr < 0x10000) return nullptr;
    for (size_t i = 0; i < s->spans.size() && i < s->bytes.size(); ++i) {
        const Span& sp = s->spans[i];
        if (addr >= sp.base && addr + 4 <= sp.base + sp.size && addr - sp.base + 4 <= s->bytes[i].size()) {
            memcpy(value, s->bytes[i].data() + (addr - sp.base), 4);
            return sp.name;
        }
    }
    return nullptr;
}

namespace {
// LoadAB's per-span name tests, made once per span name (rc25) instead of ~25 strcmp/strncmp per span (~2,200 spans a load
// on prop-heavy stages). Each flag is exactly the expression the span loop used before.
struct SpanNameFlags {
    bool hwpads, mkmaster, script, skelp, proprot, propl2w, skel, skelM, namemap, complist, scratch, fxactor, mkactor, costdmg,
        movereq, ue3anim, fightercomp, scriptModData, compArrays, skelbound, mkhelper, xblock, actor, aipad;
};
SpanNameFlags FlagsOfName(const char* n) {
    SpanNameFlags f{};
    f.hwpads = !strcmp(n, "hwpads");
    f.mkmaster = !strcmp(n, "mkmaster");
    f.script = !strncmp(n, "script", 6);
    f.skelp = !strncmp(n, "skelp_", 6);
    f.proprot = !strcmp(n, "proprot");
    f.propl2w = !strcmp(n, "propl2w");
    f.skel = !strncmp(n, "skel", 4);
    f.skelM = !strncmp(n, "skel_m", 6) || !strncmp(n, "skelp_m", 7);
    f.namemap = !strncmp(n, "namemap_", 8);
    f.complist = !strcmp(n, "complist");
    f.scratch = !strcmp(n, "script_scratch");
    f.fxactor = !strcmp(n, "fxactor");
    f.mkactor = !strcmp(n, "mkactor");
    f.costdmg = !strcmp(n, "costdmg");
    f.movereq = !strcmp(n, "movereq");
    f.ue3anim = !strcmp(n, "ue3anim");
    f.fightercomp = !strcmp(n, "fightercomp");
    f.scriptModData = !strcmp(n, "script_module") || !strcmp(n, "script_data");
    f.compArrays = !strcmp(n, "compbound") || !strcmp(n, "compweights");
    f.skelbound = !strcmp(n, "skelbound");
    f.mkhelper = !strcmp(n, "mkhelper");
    f.xblock = !strcmp(n, "xblock");
    f.actor = !strncmp(n, "actor", 5);
    f.aipad = !strcmp(n, "aipad");
    return f;
}
// LOADAB PROFILE's per-name accumulator (first: QPC ticks, second: spans), with the name's flags.
struct SpanAcc {
    int64_t first = 0;
    uint32_t second = 0;
    bool flagged = false;
    SpanNameFlags f{};
};
}  // namespace

int LoadAB(const StateAB& st, bool force, char* why, size_t whyLen) {
    LARGE_INTEGER loadStart;
    QueryPerformanceCounter(&loadStart);
    const int64_t gLoadStartQpc = loadStart.QuadPart;
    gScanN = 0;  // fresh region cache for this load
    gLastLoaded = &st;
    gMkLocN = 0;
    gXblockSkipped = 0;
    gSkipNames.clear();
    gHelperLastN = gHelperLastSpans = 0;
    gHelperRestored.clear();
    MemQuery::Scope memScope;
    static bool namer = false;
    if (!namer) {
        CrashLog::SetRegionNamer(NameRestored);
        CrashLog::SetPtrHistory([](uint32_t a, char* out, unsigned n) {
            uint32_t freed = 0, released = 0, caller = 0, block = 0;
            // The match is any block starting up to 0x400 below a: name it, it may be a neighbour (netplay
            // 2026-10-27: a live particle actor reported as a temp array freed just below it).
            if (FreeQuarantine::RecentlyReleased(reinterpret_cast<const void*>(a), 0x400, &freed, &released, &caller, &block))
                snprintf(out, n, "quarantine-released block %08X%s (freed gf=%u released gf=%u caller %08X)", block,
                         block == a ? "" : " (a neighbour?)", freed, released, caller);
        });
        CrashLog::SetSavedDwords([](uint32_t a, uint32_t* out, unsigned n) {
            const StateAB* s = gLastLoaded;
            if (!s) return false;
            for (size_t i = 0; i < s->spans.size() && i < s->bytes.size(); ++i) {
                const Span& sp = s->spans[i];
                if (a < sp.base || a + n * 4 > sp.base + sp.size || a + n * 4 - sp.base > s->bytes[i].size()) continue;
                memcpy(out, s->bytes[i].data() + (a - sp.base), n * 4);
                return true;
            }
            return false;
        });
        namer = true;
    }
    const uint32_t ids = Read32(MKHeap::kObjectIdCounter);
    if (!force && ids != st.idCounter) {
        snprintf(why, whyLen, "objects created since save (id counter %08X -> %08X)", st.idCounter, ids);
        return -1;
    }
    if ((int32_t)Read32(MKHeap::kScriptDepth) != -1) {
        snprintf(why, whyLen, "script VM not idle");
        return -1;
    }
    const uint32_t hostFrame = Read32(0x00E89BDC);
    // Fighter name-map headers as they are now (see SaveAB): kept if the restored header names other blocks.
    // One decision per map: blocks and header are restored together only if both blocks are still the saved ones
    // (synctest45: the hash block had been reallocated, the elements block was restored anyway and the kept live
    // header indexed past it -> crash in the lookup 0x00443BC9).
    // Every saved map (all MK skeletal objects since 2026-11-05) gets one decision before anything is written:
    //  restore: the image's header with the four saved blocks. Each saved block must still be this map's (the live
    //    header names it) or be held by the free quarantine (the save lists name-map blocks as restore blocks, so a
    //    rehash or the object's death in the abandoned future only defers their frees; OnLoad makes them live again).
    //  keep live: the same object (same MK id) was alive before the load and a block is neither: header and blocks
    //    stay the abandoned future's, consistent with each other (a lazily filled name cache).
    //  empty: the object died in the abandoned future and a block is gone: the image's header would name freed blocks.
    struct MapHdr {
        uintptr_t obj;
        uint8_t bytes[0x3C];
        uint8_t how;  // 0 restore, 1 keep live, 2 empty
    };
    static std::vector<MapHdr> liveMaps;
    liveMaps.clear();
    static uint32_t sMapHow[4];
    if (gNameMaps && !st.mapObjs.empty()) {
        static std::unordered_map<uint32_t, size_t> spanOf;  // namemap span identity -> span index
        spanOf.clear();
        for (size_t i = 0; i < st.spans.size(); ++i)
            if (!strncmp(st.spans[i].name, "namemap_", 8)) spanOf[st.spans[i].identity] = i;
        static const uint32_t kField[4][2] = {{0x138, 0x00}, {0x144, 0x0C}, {0x150, 0x18}, {0x16C, 0x34}};  // header cell, header offset
        for (size_t k = 0; k + 1 < st.mapObjs.size(); k += 2) {
            const uint32_t obj = st.mapObjs[k], id = st.mapObjs[k + 1];
            MapHdr m{};
            m.obj = obj;
            // Read-only here (the header is written after the span loop, behind Writable): guarded reads (rc25: ~83 uncached
            // VirtualQuery calls a load on a prop-heavy stage, most of LoadAB's 'setup').
            const bool alive = ReadableForCheck(obj + 0x138, 0x3C) && Read32(obj + 4) == id;
            if (alive) memcpy(m.bytes, reinterpret_cast<const void*>(obj + 0x138), 0x3C);
            bool same = alive, restorable = true;
            for (const auto& f : kField) {
                const auto it = spanOf.find(obj + f[0]);
                const Span* s = it == spanOf.end() ? nullptr : &st.spans[it->second];
                const uint32_t saved = s ? (uint32_t)s->base : 0;
                uint32_t live = 0xFFFFFFFF;
                if (alive) memcpy(&live, m.bytes + f[1], 4);
                if (saved != live) same = false;
                if (!saved) continue;
                uint32_t info[2] = {0, 0};
                const bool ours = (alive && saved == live) || FreeQuarantine::IsPending(reinterpret_cast<const void*>(saved));
                if (!ours || !LiveBlockInfo(reinterpret_cast<const void*>(saved), info) || info[1] < s->size) restorable = false;
            }
            m.how = (same || restorable) ? 0 : alive ? 1 : 2;
            ++sMapHow[same ? 3 : m.how];
            liveMaps.push_back(m);
            if (m.how == 2) {
                static int sEmptied = 0;
                if (sEmptied++ < 10)
                    Logger::Info("NAMEMAP %08X: the object died after the save and its map blocks are gone: emptied after the restore", obj);
            }
        }
        static uint32_t sDecisions = 0;
        if ((sDecisions += (uint32_t)(st.mapObjs.size() / 2)) % 20000 < st.mapObjs.size() / 2)
            Logger::Info("NAMEMAP loads: %u maps unchanged, %u restored from held blocks, %u kept live, %u emptied", sMapHow[3], sMapHow[0],
                         sMapHow[1], sMapHow[2]);
    }
    // LOADAB PROFILE's setup in parts (rc25): name-map decisions, external lists, script checks + heap locks + online
    // memory, the medium-heap free-tree walk.
    static int64_t sSetupPart[4] = {};
    int64_t tPart = FramePace::Now();
    sSetupPart[0] += tPart - gLoadStartQpc;
    auto setupMark = [&](int k) {
        const int64_t t = FramePace::Now();
        sSetupPart[k] += t - tPart;
        tPart = t;
    };
    // External node lists that exist now but not in the snapshot belong to objects created
    // after the save: their nodes go back to the pool with the image, so they must end empty.
    static std::vector<uintptr_t> liveExt;
    {
        MKHeap::Region mr[8];
        const int nmr = MKHeap::MasterRanges(mr, 8);
        // Its per-list Writable only decides which lists are candidates; each one emptied below is checked with
        // Writable (VirtualQuery) right before the write (rc25).
        gProbeFast = gFastProbe && CrashLog::SafeReadAvailable();
        ExternalLists(mr, nmr, liveExt);
        gProbeFast = false;
    }
    setupMark(1);
    // Script modules are checked before anything is written (the master image restores the contexts'
    // pointers to them): still an allocated GMalloc block of the size seen at the save. A module freed
    // after the save through a path the quarantine does not defer had its bytecode written over the
    // medium heap's free tree (free-tree link = opcode handler 0x00458AC0; 0x0042AF1D, 2026-10-15).
    static std::vector<uint8_t> scriptOk;
    scriptOk.assign(st.spans.size(), 1);
    for (size_t i = 0; i < st.spans.size(); ++i) {
        const Span& s = st.spans[i];
        if (strcmp(s.name, "script_module") && strcmp(s.name, "script_data")) continue;
        uint32_t info[2] = {0, 0};
        scriptOk[i] = LiveBlockInfo(reinterpret_cast<const void*>(s.base), info) && info[1] == s.identity;
    }
    int restored = 0;
    MKHeap::Region mr[8];
    const int nmr = MKHeap::MasterRanges(mr, 8);
    HeapLocks heapLocks;  // until LoadAB returns: no other thread allocates while the heaps are rewound
    MemQuery::LockedScope memLocked;  // VirtualQuery answers cached while the heaps are locked (released first)
    // MK9's online memory (heap #5): copied here and measured or put back at the end, both under the heap locks.
    OnlineKeep::BeforeLoad(st);
    setupMark(2);
    // Free chunks of GMalloc's medium heap (tree at [0x00E0AC18+0x34]; node = chunk {+4 size|flags, +0xC/+0x10 kids}).
    // A content span overlapping one would write over the free tree: the block was freed (and maybe coalesced) since
    // the save, and the header-based LiveBlockInfo check can be fooled by a merged chunk's stale interior header
    // (synctest110: post-load audit found a medium free-tree child inside a restored script_data span).
    static std::vector<std::pair<uint32_t, uint32_t>> freeRanges;
    freeRanges.clear();
    {
        static uint32_t stack[4096];
        int sp = 0;
        uint32_t root = 0;
        CrashLog::SafeRead32(0x00E0AC18 + 0x34, &root);
        if (root) stack[sp++] = root;
        while (sp > 0 && freeRanges.size() < 200000) {
            const uint32_t nd = stack[--sp];
            uint32_t sz = 0, l = 0, r = 0;
            if (!CrashLog::SafeRead32(nd + 4, &sz) || !CrashLog::SafeRead32(nd + 0xC, &l) || !CrashLog::SafeRead32(nd + 0x10, &r)) break;
            freeRanges.push_back({nd, nd + (sz & ~7u)});
            if (l && sp < 4096) stack[sp++] = l;
            if (r && sp < 4096) stack[sp++] = r;
        }
        std::sort(freeRanges.begin(), freeRanges.end());
    }
    setupMark(3);
    static uint64_t sFreeNodes = 0;  // free-tree nodes walked (LOADAB PROFILE)
    sFreeNodes += freeRanges.size();
    auto overlapsFree = [&](uint32_t b, uint32_t sz) {
        auto it = std::upper_bound(freeRanges.begin(), freeRanges.end(), std::make_pair(b + sz, 0u));
        while (it != freeRanges.begin()) {
            --it;
            if (it->second <= b) return false;  // sorted by start, chunks do not overlap
            if (it->first < b + sz) return true;
        }
        return false;
    };
    static uint32_t freeSkips = 0;
    uint32_t propOk[64];  // prop components whose matrix-array headers were restored (identifies them for propl2w)
    int propOkN = 0;
    // Load cost by span name (LOADAB PROFILE every 100 loads; the whole load cost ~48 ms, 2026-11-04).
    static std::unordered_map<const char*, SpanAcc> sSpanT;
    static int64_t sSetupT = 0, sTailT = 0;
    static int sProfLoads = 0;
    auto qpc = [] {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (int64_t)t.QuadPart;
    };
    const int64_t tLoop = qpc();
    sSetupT += tLoop - gLoadStartQpc;
    struct SpanTimer {
        SpanAcc& acc;
        int64_t t0;
        ~SpanTimer() {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            acc.first += t.QuadPart - t0;
            ++acc.second;
        }
    };
    for (size_t i = 0; i < st.spans.size(); ++i) {
        const Span& s = st.spans[i];
        SpanAcc& acc = sSpanT[s.name];
        if (!acc.flagged) {
            acc.f = FlagsOfName(s.name);
            acc.flagged = true;
        }
        const SpanNameFlags& nf = acc.f;
        SpanTimer spanTimer{acc, qpc()};
        if (!Writable(s.base, s.size)) {
            // A span that cannot be written back is state this load does not rewind: said in the log (it was skipped in
            // silence until rc30, so a desync from it had no trace), the first 20 and then every 256th.
            static uint32_t unwritable = 0;
            if (++unwritable <= 20 || !(unwritable & 0xFF))
                Logger::Info("LOADAB skip %s %08X+%X: no longer writable (unmapped or protected since the save), %u so far", s.name,
                             (unsigned)s.base, s.size, unwritable);
            continue;
        }
        uint32_t padHw[kHwPadSlots];
        const bool padHwKeep = gPadHwLive && s.size >= kHwPadSlots * kPadObjSize && nf.hwpads;
        if (padHwKeep)
            for (int p = 0; p < kHwPadSlots; ++p) padHw[p] = Read32(s.base + p * kPadObjSize + 8);
        if (s.base >= 0x01000000 && !nf.mkmaster && overlapsFree((uint32_t)s.base, s.size)) {
            if (freeSkips++ < 20)
                Logger::Info("LOADAB skip %s %08X+%X: overlaps a free medium-heap chunk (freed since the save)", s.name, (unsigned)s.base,
                             s.size);
            continue;
        }
        // A content block the quarantine really freed after this save may already belong to someone else (the size-only
        // identity check passes for any reuse of the same size class): never write the old contents into it.
        if (nf.script) {  // the release ring is scanned linearly: only the few script spans
            uint32_t fg = 0, rg = 0, caller = 0;
            if (FreeQuarantine::RecentlyReleased(reinterpret_cast<const void*>(s.base), 1, &fg, &rg, &caller, nullptr, st.gameFrame) &&
                rg >= st.gameFrame &&
                rg - st.gameFrame < 0x10000) {
                static uint32_t scriptSkips = 0;  // counted on its own: the shared 20-line cap hid them (soak 11)
                ++scriptSkips;
                if (freeSkips++ < 20 || scriptSkips % 200 == 1)
                    Logger::Info("LOADAB skip %s %08X+%X: released by the quarantine at gf %u (freed at gf %u by %08X) after the save gf %u"
                                 " (script spans skipped so far: %u)",
                                 s.name, (unsigned)s.base, s.size, rg, fg, caller, st.gameFrame, scriptSkips);
                continue;
            }
        }
        if (nf.skelp && !gPropSkelLoad) continue;
        // A prop component's matrix-array headers: restored only when they still name the same three blocks (a rotation
        // of them), which also identifies the component for its LocalToWorld below (same order of spans as saved).
        if (nf.proprot || nf.propl2w) {
            if (!gPropSkelLoad || Read32(s.identity) != kSkelMeshVtable) {
                ++gPropSkelSkipped;
                continue;
            }
            if (nf.proprot) {
                uint32_t sv[3][3], lv[3][3];
                memcpy(sv, st.bytes[i].data(), sizeof sv);
                memcpy(lv, reinterpret_cast<const void*>(s.base), sizeof lv);
                bool perm = true;
                for (int a = 0; a < 3 && perm; ++a) {
                    bool found = false;
                    for (int b = 0; b < 3; ++b) found |= sv[a][0] && sv[a][0] == lv[b][0] && sv[a][1] == lv[b][1] && sv[a][2] == lv[b][2];
                    perm = found;
                }
                if (!perm) {
                    ++gPropSkelSkipped;
                    continue;
                }
                if (propOkN < 64) propOk[propOkN++] = s.identity;
            } else {
                if (std::find(propOk, propOk + propOkN, s.identity) == propOk + propOkN) {
                    ++gPropSkelSkipped;
                    continue;
                }
                if (++gPropSkelRestored == 1 || gPropSkelRestored % 1000 == 0)
                    Logger::Info("PROPSKEL: %u prop component poses restored, %u spans skipped (%u saved)", gPropSkelRestored,
                                 gPropSkelSkipped, gPropSkelSaved);
            }
        }
        if (nf.skel) {
            uint32_t info[2] = {0, 0};
            // The three matrix arrays rotate the same three blocks every frame (see the fightercomp merge, which
            // restores the saved rotation): a matrix block is still the component's if it is any of the three.
            bool owned = Read32(s.identity) == s.base;
            if (!owned && gBoneRotation && nf.skelM) {
                const char k = s.name[strlen(s.name) - 1];
                const uint32_t off = k == '0' ? 0x2F4 : k == '1' ? 0x300 : 0x30C;
                const uint32_t comp = s.identity - off;
                owned = Writable(comp + 0x2F4, 0x24) &&
                        (Read32(comp + 0x2F4) == s.base || Read32(comp + 0x300) == s.base || Read32(comp + 0x30C) == s.base);
            }
            if (!owned || !LiveBlockInfo(reinterpret_cast<const void*>(s.base), info) || info[1] < s.size)
                continue;  // the component reallocated the array: the saved block is not its data any more
        }
        if (nf.namemap) {
            // The map's decision (above, made before anything was written; it checked these blocks too).
            bool restore = false;
            for (const MapHdr& m : liveMaps)
                if (m.obj && (s.identity == m.obj + 0x138 || s.identity == m.obj + 0x16C || s.identity == m.obj + 0x144 ||
                              s.identity == m.obj + 0x150))
                    restore = m.how == 0;
            uint32_t info[2] = {0, 0};
            if (!restore || !LiveBlockInfo(reinterpret_cast<const void*>(s.base), info) || info[1] < s.size) {
                SkipSpan(s.name);
                continue;
            }
        }
        if (nf.complist) {
            uint32_t info[2] = {0, 0};
            // The list emptied and freed its block after the save (live header null): the block is still held by the
            // free quarantine, which gives it back at OnLoad because the restored component header references it.
            const bool rescue = gListRescue && Read32(s.identity) == 0 && FreeQuarantine::IsPending(reinterpret_cast<const void*>(s.base));
            if (!rescue && (Read32(s.identity) != s.base || !LiveBlockInfo(reinterpret_cast<const void*>(s.base), info) || info[1] < s.size)) {
                SkipSpan(s.name);
                continue;
            }
        }
        if (nf.scratch) {
            // [0x00E39FC4] is live (not in Tier A): the executor scratch must still be this block.
            uint32_t info[2] = {0, 0};
            if (Read32(0x00E39FC4) != s.base || !LiveBlockInfo(reinterpret_cast<const void*>(s.base), info) || info[1] < s.size) {
                SkipSpan(s.name);
                continue;
            }
        }
        if (nf.fxactor && !gFxActorsLoad) continue;  // 'abtest fxactors'
        if (nf.mkactor && !gMkActorsLoad) {  // 'abtest mkactors'
            NoteMkLoc((uint32_t)s.base - 0x3C, 3, nullptr);
            continue;
        }
        if (nf.costdmg && (!gCostDmgLoad || !Writable(s.identity, 8) || Read32(s.identity) != s.base ||
                                           Read32(s.identity + 4) * 0x14 != s.size || !Writable(s.base, s.size))) {
            SkipSpan(s.name);  // 'abtest costdmg', or the group reallocated its pieces since the save
            continue;
        }
        if (nf.movereq) {
            bool listed = false;  // the fighter (identity) must still list this entry
            const uint32_t cnt = Writable(s.identity + 0x7524, 0xCC) ? Read32(s.identity + 0x75EC) : 0;
            for (uint32_t k = 0; k < cnt && k < 50 && !listed; ++k) listed = Read32(s.identity + 0x7524 + 4 * k) == s.base;
            if (!gMoveReqLoad || !listed || !Writable(s.base, s.size)) {
                SkipSpan(s.name);  // 'abtest movereq'
                continue;
            }
        }
        const bool fxspan = nf.fxactor || nf.mkactor;
        if (nf.ue3anim || nf.fightercomp || fxspan) {
            // Tier E UE3 objects (restored from +0x3C): the object must still be an allocated GMalloc block
            // covering the span; a garbage-collected object's memory may hold allocator links or another object.
            uint32_t info[2] = {0, 0};
            const uintptr_t obj = s.base - 0x3C;
            if (!LiveBlockInfo(reinterpret_cast<const void*>(obj), info) || info[1] < s.size + 0x3C ||
                ((nf.ue3anim || fxspan) && (!LiveUObject(obj) || Read32(obj + 4) != s.identity)) ||
                (fxspan && (Read32(obj + 0x80) & 0x20))) {  // bDeleteMe: destroyed since the save
                if (nf.mkactor) NoteMkLoc((uint32_t)obj, Writable(obj + 0x80, 4) && (Read32(obj + 0x80) & 0x20) ? 2 : 1, nullptr);
                SkipSpan(s.name);
                continue;
            }
            if (nf.fxactor) ++gFxActorRestored;
            if (nf.mkactor) {
                ++gMkActorRestored;
                NoteMkLoc((uint32_t)obj, 9, nullptr);  // becomes 0 once written
            }
        }
        if (nf.scriptModData && !scriptOk[i]) {
            SkipSpan(s.name);
            continue;
        }
        if (nf.compArrays &&
            (!gCompWeightsLoad || !Writable(s.identity, 4) || Read32(s.identity) != s.base)) {
            SkipSpan(s.name);  // the component reallocated the array since the save
            continue;
        }
        if (nf.skelbound && (!gSkelBoundLoad ||
            !Writable(s.base, 0x60) || Read32(s.base) != kSkelBoundVtable || Read32(s.base + 0xC) != s.identity)) {
            SkipSpan(s.name);  // element moved, reused or unbound since the save
            continue;
        }
        if (nf.mkhelper) {
            // Restored only while the owner still points at the same live helper block.
            uint32_t info[2] = {0, 0}, cell = 0;
            uint8_t why = 0;
            if (!gMkHelpersLoad) why = 1;
            else if (!Writable(s.base, kMkHelperSize)) why = 2;
            else if (Read32(s.base) != kMkHelperVtable &&
                     !(gMkHelperRevive && Read32(s.base) == kMkHelperDeadVtable &&
                       FreeQuarantine::IsPending(reinterpret_cast<const void*>(s.base))))
                why = 3;  // destroyed after the save: revived only while the quarantine still holds the block
            else if (!CrashLog::SafeRead32(s.identity + 0x1D8, &cell) || cell != (uint32_t)s.base) why = 4;
            else if (!LiveBlockInfo(reinterpret_cast<const void*>(s.base), info) || info[1] < kMkHelperSize) why = 5;
            ++gHelperLastSpans;
            if (gHelperLastN < sizeof gHelperLast / sizeof gHelperLast[0]) {
                HelperOutcome& o = gHelperLast[gHelperLastN++];
                o = {s.identity, (uint32_t)s.base, cell, 0, 0xFFFFFFFF, why};
                if (st.bytes[i].size() >= 0x10) memcpy(&o.savedC, st.bytes[i].data() + 0xC, 4);
                CrashLog::SafeRead32(s.base + 0xC, &o.liveC);
            }
            if (why) {
                SkipSpan(s.name);
                continue;
            }
            ++gMkHelperRestored;
            gHelperRestored.push_back((uint32_t)s.base);
            if (Read32(s.base) == kMkHelperDeadVtable && (++gMkHelperRevived <= 20 || gMkHelperRevived % 200 == 0))
                Logger::Info("MKHELPER revived #%u: helper %08X of owner %08X destroyed after the save (+0xC saved %08X live %08X)",
                             gMkHelperRevived, (uint32_t)s.base, s.identity, gHelperLastN ? gHelperLast[gHelperLastN - 1].savedC : 0,
                             gHelperLastN ? gHelperLast[gHelperLastN - 1].liveC : 0);
        }
        if (nf.xblock) {
            // A block freed after the save by a path the quarantine does not defer holds allocator
            // links now; writing the saved bytes over them corrupted an MK heap free tree (soak31,
            // 0x0042AF1D, after blend-node mask arrays were added as xblocks).
            uint32_t info[2] = {0, 0};
            if (!LiveBlockInfo(reinterpret_cast<const void*>(s.base), info) || info[1] < s.size) {
                SkipSpan(s.name);
                continue;
            }
        }
        if (!s.keepOffsets.empty()) {  // actors: pointer-safe merge
            std::vector<uint8_t> merged = st.bytes[i];
            static int sCompKeepLogged = 0, sArrLogged = 0;  // COMPKEEP / ACTORARRAY lines (both merges below)
            static uint32_t sCompLiveLogged = 0;              // COMPLIVEDATA lines
            // The whole-span merge (every load before rc25; 'diffmerge 0', the diff merge's fallback and its 'diffmerge 2'
            // check). 'quiet': no counters or log lines (the check).
            auto fullMerge = [&](std::vector<uint8_t>& mg, bool quiet) {
            // Fields that are pointers now are kept live too, not only those that were pointers at
            // the save: a fighter's attachment TArray that was empty (null) at S was restored to null
            // while the other fighter's Base, a pointer, stayed live -> Base without a record
            // (grab during the window; resim crash 0x007898E5, correction test 2026-10-15).
            // UE3 actor spans only: in MK object spans (char*, pinfo*) MK ids 0x1000xxxx look like heap
            // pointers, and keeping them live put an id into an MK heap free tree (0x0042AF1D).
            std::vector<uint32_t> ko = s.keepOffsets;
            // The fighters' attached actors too: an effect actor with no children at S had its AttachedActors
            // TArray (+0x68) restored to {null, 0, 0} while a child's Base, a pointer, stayed live -> the base-record
            // lookup 0x007896F0 returned null+4 (resim crash 0x0043E117, then 0x00793E0E; writer_2 2026-10-18).
            if (!strncmp(s.name, "actor", 5) || !strcmp(s.name, "fxactor") || !strcmp(s.name, "mkactor")) {
                const std::vector<uint32_t> liveKo = PointerLikeOffsets(s.base, s.size);
                ko.insert(ko.end(), liveKo.begin(), liveKo.end());
                std::sort(ko.begin(), ko.end());
                ko.erase(std::unique(ko.begin(), ko.end()), ko.end());
                // Location (+0xDC/+0xE0/+0xE4) is a float vector, never a pointer, but a coordinate such as
                // 109.9 = 0x42DBD61C is also an aligned heap address, and when that address happened to start an
                // object with a vtable the heuristic kept the abandoned future's coordinate (synctest59: P2 Z at
                // the first replayed frame's FightUpdate entry 178.2 instead of 109.9, MK state identical).
                // fxactor/mkactor spans start at +0x3C: the same fields at span offsets 0xA0..0xB4.
                const uint32_t lo = s.name[0] == 'a' ? 0xDC : 0xA0;
                if (gFloatFields)
                    ko.erase(std::remove_if(ko.begin(), ko.end(), [lo](uint32_t o) { return o >= lo && o <= lo + 0x14; }), ko.end());  // + Rotation
            }
            // SkelControlPinLimb float vectors +0x90..+0x98 and +0xA8..+0xB0 (span offsets 0x54..0x5C, 0x6C..0x74): animtrace found
            // them differing on first replayed frames (12-13 of ~25 node differences each) - floats that the pointer heuristic
            // may keep live. Always restored by value ('abtest pinfloats').
            // Declared data fields of UE3 objects are never kept live (reflection, ReflectDataMask).
            const bool ue3span = !strcmp(s.name, "ue3anim") || !strcmp(s.name, "fightercomp") || !strcmp(s.name, "fxactor") || !strcmp(s.name, "mkactor") || !strncmp(s.name, "actor", 5);
            if (gReflectData && ue3span && !ko.empty()) {
                const uint32_t delta = strncmp(s.name, "actor", 5) ? 0x3C : 0;  // span start inside the object
                uint32_t cls = 0;
                if (CrashLog::SafeRead32(s.base - delta + 0x30, &cls) && cls) {
                    const std::vector<uint8_t>& dm = ReflectDataMask(cls);
                    const size_t before = ko.size();
                    ko.erase(std::remove_if(ko.begin(), ko.end(),
                                            [&](uint32_t o) {
                                                const uint32_t w = (o + delta) / 4;
                                                return w < dm.size() && dm[w];
                                            }),
                             ko.end());
                    if (!quiet) {
                        gReflectRestored += (uint32_t)(before - ko.size());
                        if (gReflectRestored >= gReflectLoggedAt + 500) {
                            gReflectLoggedAt = gReflectRestored;
                            Logger::Info("REFLECTDATA: %u declared-data dwords restored by value instead of kept live", gReflectRestored);
                        }
                    }
                }
            }
            // Fighter-component float vectors the pointer test can take for heap addresses (COMPKEEP, soaks 11701-12109:
            // unaligned or negative values such as 428DB6E7, B2A44588, 4279F84A were kept live) - restored by value.
            if (gCompFloats && !strcmp(s.name, "fightercomp") && !ko.empty()) {
                const size_t before = ko.size();
                ko.erase(std::remove_if(ko.begin(), ko.end(),
                                        [](uint32_t o) {
                                            const uint32_t co = o + 0x3C;
                                            return (co >= 0xA0 && co <= 0xA8) || (co >= 0xE0 && co <= 0xE8) ||
                                                   (co >= 0x5E0 && co <= 0x5E8) || (co >= 0x60C && co <= 0x614);
                                        }),
                         ko.end());
                if (!quiet && ko.size() != before && (++gCompFloatsRestored == 1 || gCompFloatsRestored % 500 == 0))
                    Logger::Info("COMPFLOATS: fighter component float vectors restored by value instead of kept live (%u loads)", gCompFloatsRestored);
            }
            // A fighter-component dword kept because its SAVED value passed the pointer test is data when its LIVE value
            // is nonzero and fails it: a real pointer field holds a pointer or null. Restored by value. Regression r2
            // 2026-11-04: a UObject at 0x3F800000 made 1.0f a "pointer", so +0x74 and +0xC0 (1.0 at the save, -1.0 live:
            // the fighter turned around in the window) and +0x608 (1.0 / 0.99999994) kept the abandoned timeline's values
            // at load S=100682 -> P2 position desync from frame 683 to the fight's end (1254 frames).
            if (gCompLiveData && !strcmp(s.name, "fightercomp") && !ko.empty()) {
                std::vector<uint8_t> liveKeep(s.size / 4, 0);
                MarkKeep(reinterpret_cast<const uint8_t*>(s.base), s.size, liveKeep);
                uint32_t firstO = 0, firstS = 0, firstL = 0;
                const size_t before = ko.size();
                ko.erase(std::remove_if(ko.begin(), ko.end(),
                                        [&](uint32_t o) {
                                            uint32_t sv, lv;
                                            memcpy(&sv, mg.data() + o, 4);
                                            memcpy(&lv, reinterpret_cast<const void*>(s.base + o), 4);
                                            const bool data = sv != lv && lv && o / 4 < liveKeep.size() && !liveKeep[o / 4];
                                            if (data && !firstO) firstO = o + 0x3C, firstS = sv, firstL = lv;
                                            return data;
                                        }),
                         ko.end());
                if (!quiet && ko.size() != before) {
                    gCompLiveDataRestored += (uint32_t)(before - ko.size());
                    if (++sCompLiveLogged <= 20 || sCompLiveLogged % 200 == 0)
                        Logger::Info("COMPLIVEDATA S=%u: %u fighter component dword(s) restored by value (live value not a pointer; first "
                                     "+0x%X saved %08X live %08X; %u so far)",
                                     st.gameFrame, (unsigned)(before - ko.size()), firstO, firstS, firstL, gCompLiveDataRestored);
                }
            }
            // Diagnostic: fighter-component dwords still kept live whose saved and live values differ. Soak 11909 S=840:
            // comp +0xA8 was 3.5 (0x40600000) in the original and 51.7 (0x424EDD74) in the replay at the first replayed
            // frame's FightUpdate entry, +0x5E8 3.5 vs 0, then P2 z 0 -> 46.8: floats that pass the pointer test?
            if (!quiet && !strcmp(s.name, "fightercomp")) {
                for (uint32_t o : ko) {
                    if (sCompKeepLogged >= 300) break;
                    // Expected: the triple-buffered bone matrix pointers (+0x2F4/+0x300/+0x30C, rotated by bonerot)
                    // and the queued anim-object list header (+0x3A8..+0x3B0) differ at almost every load.
                    const uint32_t co = o + 0x3C;
                    if (co == 0x2F4 || co == 0x300 || co == 0x30C || (co >= 0x3A8 && co <= 0x3B0)) continue;
                    uint32_t sv, lv;
                    memcpy(&sv, mg.data() + o, 4);
                    memcpy(&lv, reinterpret_cast<const void*>(s.base + o), 4);
                    if (sv == lv) continue;
                    ++sCompKeepLogged;
                    Logger::Info("COMPKEEP S=%u fightercomp +0x%X kept live: saved %08X live %08X", st.gameFrame, o + 0x3C, sv, lv);
                }
            }
            if (gPinFloats && !strcmp(s.name, "ue3anim") && !ko.empty()) {
                uint32_t cls = 0;
                char cn[64] = "";
                CrashLog::SafeRead32(s.base - 0x3C + 0x30, &cls);
                if (cls) Ue3Names::NameOf(cls, cn, sizeof cn);
                if (!strncmp(cn, "SkelControlPinLimb", 18)) {
                    // (+0x40, the strength, is never in this list: traced soak 4 2026-11-05, PINSTRENGTH 0 in 4 sessions.)
                    const size_t before = ko.size();
                    ko.erase(std::remove_if(ko.begin(), ko.end(), [](uint32_t o) { return (o >= 0x54 && o <= 0x5C) || (o >= 0x6C && o <= 0x74); }),
                             ko.end());
                    if (!quiet && ko.size() != before && (++gPinFloatsRestored == 1 || gPinFloatsRestored % 500 == 0))
                        Logger::Info("PINFLOATS: PinLimb vector fields restored by value instead of kept live (%u times)", gPinFloatsRestored);
                }
            }
            for (size_t k = 0; k < ko.size(); ++k) {
                const uint32_t o = ko[k];
                // A pointer into the master-heap image is restored with the image: keeping the
                // live value would leave it pointing at a block the image just made free (e.g. an
                // MK object created after the save) and later writes through it corrupt the heap
                // free tree (soak12: an MK object id in a free-tree link, 0x0042AF1D).
                uint32_t saved, live;
                memcpy(&saved, mg.data() + o, 4);
                memcpy(&live, reinterpret_cast<const void*>(s.base + o), 4);
                bool intoImage = false;
                for (int r = 0; r < nmr; ++r)
                    intoImage |= (saved >= mr[r].base && saved < mr[r].base + mr[r].size) ||
                                 (live >= mr[r].base && live < mr[r].base + mr[r].size);
                // A TArray {data, num, max} is marked as three consecutive offsets: decide once for
                // all three (a restored data pointer with a live count gave a null array with
                // num > 0 -> crash 0x007892AB, soak15).
                const bool triple = k + 2 < ko.size() && ko[k + 1] == o + 4 && ko[k + 2] == o + 8;
                const uint32_t n = triple ? 3 : 1;
                if (!quiet && triple && !strncmp(s.name, "actor", 5)) {
                    // Diagnostic: actor TArrays kept live whose count differs from the save (Touching etc.).
                    uint32_t sn, ln;
                    memcpy(&sn, mg.data() + o + 4, 4);
                    memcpy(&ln, reinterpret_cast<const void*>(s.base + o + 4), 4);
                    if (sn != ln && sArrLogged < 2000) {
                        ++sArrLogged;
                        Logger::Info("ACTORARRAY S=%u %s +0x%X: count %u at the save, %u live (kept live)", st.gameFrame, s.name, o, sn, ln);
                    }
                }
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t oj = o + 4 * j;
                    // A field learned to hold data (a float that sometimes coincides with an object address) is restored.
                    if (n == 1 && LearnFieldsOn() && LearnedData((uint32_t)(s.base + oj))) {
                        if (!quiet) {
                            ++gLearnedRestores;
                            char key[48];
                            snprintf(key, sizeof key, "%.12s +0x%X", s.name, oj);
                            auto& e = gLearnedWhere[key];
                            ++e.first;
                            memcpy(&e.second, mg.data() + oj, 4);
                        }
                        if (Snapshot::gLearnFields == 1) continue;
                    }
                    if (!intoImage) memcpy(mg.data() + oj, reinterpret_cast<const void*>(s.base + oj), 4);
                }
                k += n - 1;
            }
            };
            // rc25 diff-first merge ('diffmerge 1', default): the same bytes as fullMerge, classifying only what can
            // matter. A dword whose live and saved values are equal merges to that value whatever the classification,
            // so only the differing dwords are decided, each exactly as fullMerge decides it:
            //  - its kept-live membership (saved keepOffsets, the live pointer scan for actor spans, then the same
            //    filters) is computed for it alone: a live mark on dword o can only come from the origins o - 8, o - 4
            //    and o (MarkKeepOrigins), and every filter is a test of that one dword;
            //  - fullMerge groups kept dwords greedily into TArray triples along the sorted list, so a dword's group is
            //    fixed by the run of consecutive kept dwords around it: from the run's start, triples while 3 dwords
            //    remain, then singles. The run is walked (memberships of its equal dwords computed the same way), and the
            //    group's first dword decides intoImage for it, as in fullMerge.
            // The counters and lines follow fullMerge's except REFLECTDATA on actor spans (the saved keepOffsets part only:
            // the live part is no longer classified in full). Not used with LEARNFIELDS on (it learns from the full live
            // scan), for a span whose saved bytes are short, or for unsorted keepOffsets: fullMerge then.
            auto diffMerge = [&](std::vector<uint8_t>& mg) -> bool {
                const std::vector<uint32_t>& sk = s.keepOffsets;
                if (LearnFieldsOn() || st.bytes[i].size() < s.size) return false;
                for (size_t k = 0; k < sk.size(); ++k)
                    if ((sk[k] & 3) || sk[k] + 4 > s.size || (k && sk[k] <= sk[k - 1])) return false;
                const uint8_t* svb = st.bytes[i].data();
                const uint8_t* lvb = reinterpret_cast<const uint8_t*>(s.base);
                auto sv32 = [&](uint32_t o) { uint32_t v; memcpy(&v, svb + o, 4); return v; };
                auto lv32 = [&](uint32_t o) { uint32_t v; memcpy(&v, lvb + o, 4); return v; };
                static std::vector<uint32_t> diff, origins;  // game thread only (LoadAB)
                static std::vector<uint8_t> liveKeep, scanned;
                static std::vector<int8_t> memo;
                diff.clear();
                for (uint32_t o = 0; o + 4 <= s.size; o += 4)
                    if (sv32(o) != lv32(o)) diff.push_back(o);
                ++gDiffSpans;
                gDiffDwords += (uint32_t)diff.size();
                if (diff.empty()) ++gDiffUnchanged;
                const bool actorType = !strncmp(s.name, "actor", 5) || !strcmp(s.name, "fxactor") || !strcmp(s.name, "mkactor");
                const bool isComp = !strcmp(s.name, "fightercomp"), isAnim = !strcmp(s.name, "ue3anim");
                const bool ue3span = isAnim || isComp || actorType;
                const uint32_t floatLo = s.name[0] == 'a' ? 0xDC : 0xA0;
                const std::vector<uint8_t>* dm = nullptr;
                uint32_t delta = 0;
                if (gReflectData && ue3span) {
                    delta = strncmp(s.name, "actor", 5) ? 0x3C : 0;  // span start inside the object
                    uint32_t cls = 0;
                    if (CrashLog::SafeRead32(s.base - delta + 0x30, &cls) && cls) dm = &ReflectDataMask(cls);
                }
                auto inSaved = [&](uint32_t o) { return std::binary_search(sk.begin(), sk.end(), o); };
                auto floatOut = [&](uint32_t o) { return actorType && gFloatFields && o >= floatLo && o <= floatLo + 0x14; };
                auto reflectOut = [&](uint32_t o) {
                    const uint32_t w = (o + delta) / 4;
                    return dm && w < dm->size() && (*dm)[w];
                };
                auto compFloatOut = [&](uint32_t o) {
                    const uint32_t co = o + 0x3C;
                    return isComp && gCompFloats &&
                           ((co >= 0xA0 && co <= 0xA8) || (co >= 0xE0 && co <= 0xE8) || (co >= 0x5E0 && co <= 0x5E8) || (co >= 0x60C && co <= 0x614));
                };
                auto pinRange = [](uint32_t o) { return (o >= 0x54 && o <= 0x5C) || (o >= 0x6C && o <= 0x74); };
                // The live pointer scan, origin by origin (MarkKeepOrigins: the whole scan's marks on the dwords asked for).
                const uint32_t nd = s.size / 4;
                const bool needLive = actorType || (isComp && gCompLiveData);
                if (needLive) {
                    liveKeep.assign(nd, 0);
                    scanned.assign(nd, 0);
                }
                auto scanOrigins = [&](uint32_t lo, uint32_t hi) {  // the origins in [lo, hi] not scanned yet
                    origins.clear();
                    for (uint32_t p = lo; p <= hi && p + 4 <= s.size; p += 4)
                        if (!scanned[p / 4]) {
                            scanned[p / 4] = 1;
                            origins.push_back(p);
                        }
                    if (origins.empty()) return;
                    MarkKeepOrigins(lvb, s.size, origins.data(), origins.size(), liveKeep);
                    gDiffOrigins += (uint32_t)origins.size();
                };
                auto liveAt = [&](uint32_t o) {
                    scanOrigins(o >= 8 ? o - 8 : 0, o);
                    return liveKeep[o / 4] != 0;
                };
                if (actorType && !diff.empty()) {  // one scan (one working-set query) for the differing dwords and their neighbours
                    origins.clear();
                    for (const uint32_t d : diff)
                        for (uint32_t p = d >= 16 ? d - 16 : 0; p <= d + 8 && p + 4 <= s.size; p += 4)
                            if (!scanned[p / 4]) {
                                scanned[p / 4] = 1;
                                origins.push_back(p);
                            }
                    if (!origins.empty()) {
                        MarkKeepOrigins(lvb, s.size, origins.data(), origins.size(), liveKeep);
                        gDiffOrigins += (uint32_t)origins.size();
                    }
                }
                // fullMerge's list before COMPLIVEDATA / PINFLOATS: saved and (actor spans) live marks, minus Location /
                // Rotation, declared data and the component float vectors.
                auto preMember = [&](uint32_t o) {
                    const bool in = inSaved(o) || (actorType && liveAt(o));
                    return in && !floatOut(o) && !reflectOut(o) && !compFloatOut(o);
                };
                // REFLECTDATA / COMPFLOATS / PINFLOATS counters: those spans' lists are the saved keepOffsets alone (actor
                // spans: their saved part), so the counts are fullMerge's.
                bool savedLeft = false, compFloatHit = false, pinHit = false;
                uint32_t reflectHits = 0;
                for (const uint32_t o : sk) {
                    if (floatOut(o)) continue;
                    if (reflectOut(o)) {
                        ++reflectHits;
                        continue;
                    }
                    savedLeft = true;
                    compFloatHit |= compFloatOut(o);
                    pinHit |= isAnim && pinRange(o);
                }
                if (dm && reflectHits) {
                    gReflectRestored += reflectHits;
                    if (gReflectRestored >= gReflectLoggedAt + 500) {
                        gReflectLoggedAt = gReflectRestored;
                        Logger::Info("REFLECTDATA: %u declared-data dwords restored by value instead of kept live", gReflectRestored);
                    }
                }
                if (isComp && gCompFloats && compFloatHit && (++gCompFloatsRestored == 1 || gCompFloatsRestored % 500 == 0))
                    Logger::Info("COMPFLOATS: fighter component float vectors restored by value instead of kept live (%u loads)", gCompFloatsRestored);
                // COMPLIVEDATA: a kept fighter-component dword whose live value is nonzero and not a pointer (only a differing
                // dword can be one).
                if (isComp && gCompLiveData) {
                    uint32_t removed = 0, firstO = 0, firstS = 0, firstL = 0;
                    for (const uint32_t d : diff) {
                        const uint32_t lv = lv32(d);
                        if (!lv || !preMember(d) || liveAt(d)) continue;
                        if (!firstO) firstO = d + 0x3C, firstS = sv32(d), firstL = lv;
                        ++removed;
                    }
                    if (removed) {
                        gCompLiveDataRestored += removed;
                        if (++sCompLiveLogged <= 20 || sCompLiveLogged % 200 == 0)
                            Logger::Info("COMPLIVEDATA S=%u: %u fighter component dword(s) restored by value (live value not a pointer; first "
                                         "+0x%X saved %08X live %08X; %u so far)",
                                         st.gameFrame, removed, firstO, firstS, firstL, gCompLiveDataRestored);
                    }
                }
                // PINFLOATS: the class test fullMerge makes when its list is not empty (ue3anim spans have no live part).
                bool pinLimb = false;
                if (isAnim && gPinFloats && savedLeft) {
                    uint32_t cls = 0;
                    char cn[64] = "";
                    CrashLog::SafeRead32(s.base - 0x3C + 0x30, &cls);
                    if (cls) Ue3Names::NameOf(cls, cn, sizeof cn);
                    pinLimb = !strncmp(cn, "SkelControlPinLimb", 18);
                    if (pinLimb && pinHit && (++gPinFloatsRestored == 1 || gPinFloatsRestored % 500 == 0))
                        Logger::Info("PINFLOATS: PinLimb vector fields restored by value instead of kept live (%u times)", gPinFloatsRestored);
                }
                if (diff.empty()) return true;  // nothing differs: the merge is the saved bytes, as fullMerge's would be
                memo.assign(nd, -1);
                auto member = [&](uint32_t o) {  // in fullMerge's final list
                    int8_t& m = memo[o / 4];
                    if (m < 0) {
                        bool in = preMember(o);
                        if (in && isComp && gCompLiveData) {
                            const uint32_t lv = lv32(o);
                            if (sv32(o) != lv && lv && !liveAt(o)) in = false;
                        }
                        if (in && isAnim && gPinFloats && pinLimb && pinRange(o)) in = false;
                        m = in ? 1 : 0;
                    }
                    return m != 0;
                };
                if (isComp) {  // COMPKEEP diagnostic: kept dwords whose values differ (fullMerge's order: ascending)
                    for (const uint32_t o : diff) {
                        if (sCompKeepLogged >= 300) break;
                        const uint32_t co = o + 0x3C;
                        if (co == 0x2F4 || co == 0x300 || co == 0x30C || (co >= 0x3A8 && co <= 0x3B0) || !member(o)) continue;
                        ++sCompKeepLogged;
                        Logger::Info("COMPKEEP S=%u fightercomp +0x%X kept live: saved %08X live %08X", st.gameFrame, o + 0x3C, sv32(o), lv32(o));
                    }
                }
                uint32_t runLo = 1, runHi = 0;  // the run of consecutive kept dwords around the last decided dword
                for (const uint32_t d : diff) {
                    if (!member(d)) continue;  // not kept live: the saved value (already in mg)
                    if (d < runLo || d > runHi) {
                        runLo = runHi = d;
                        while (runLo >= 4 && member(runLo - 4)) runLo -= 4;
                        while (runHi + 8 <= s.size && member(runHi + 4)) runHi += 4;
                    }
                    const uint32_t g = runLo + 12 * (((d - runLo) / 4) / 3);  // its group's first dword if a triple
                    const bool triple = g + 8 <= runHi;
                    const uint32_t o = triple ? g : d;
                    const uint32_t saved = sv32(o), live = lv32(o);
                    bool intoImage = false;
                    for (int r = 0; r < nmr; ++r)
                        intoImage |= (saved >= mr[r].base && saved < mr[r].base + mr[r].size) ||
                                     (live >= mr[r].base && live < mr[r].base + mr[r].size);
                    if (triple && d == g + 4 && !strncmp(s.name, "actor", 5) && sArrLogged < 2000) {  // its count differs
                        ++sArrLogged;
                        Logger::Info("ACTORARRAY S=%u %s +0x%X: count %u at the save, %u live (kept live)", st.gameFrame, s.name, g, sv32(d), lv32(d));
                    }
                    if (!intoImage) memcpy(mg.data() + d, lvb + d, 4);
                }
                return true;
            };
            if (gDiffMerge <= 0 || !diffMerge(merged)) {
                if (gDiffMerge > 0) ++gDiffFull;
                fullMerge(merged, false);
            } else if (gDiffMerge >= 2) {  // check: the whole-span merge must give the same bytes
                std::vector<uint8_t> check = st.bytes[i];
                fullMerge(check, true);
                if (check != merged) {
                    uint32_t at = 0;
                    while (at < check.size() && check[at] == merged[at]) ++at;
                    if (gDiffMismatch++ < 20)
                        Logger::Info("DIFFMERGE MISMATCH S=%u %s %08X+%X: first at +0x%X (full %02X, diff %02X); the full merge is used",
                                     st.gameFrame, s.name, (unsigned)s.base, s.size, at, at < check.size() ? check[at] : 0,
                                     at < merged.size() ? merged[at] : 0);
                    merged.swap(check);
                }
            }
            if (nf.actor) KeepActorRenderFields(merged.data(), reinterpret_cast<const uint8_t*>(s.base));
            // SkelControl bound object (+0xC0, span offset 0x84): the control's strength is [bound+0x20] (0x007601B0)
            // and the binding is made by 0x0076DF10 (which also appends the object to the component's +0x3A8 list,
            // restored). Kept live as a pointer, it left the controls bound as at F: 'animtrace' found fighter
            // SkelControlPinLimb strength already at 1.0 / 0 on first replayed frames where live ramped 0.2..0.8,
            // in ~20% of loads (2026-10-17). Restored when the saved object is null or still/again allocated.
            if (gSkelBind && nf.ue3anim && s.size >= 0x88) {
                uint32_t saved, live, cls = 0;
                memcpy(&saved, st.bytes[i].data() + 0x84, 4);
                memcpy(&live, reinterpret_cast<const void*>(s.base + 0x84), 4);
                CrashLog::SafeRead32(s.base - 0x3C + 0x30, &cls);
                char cn[64] = "";
                if (saved != live && cls) Ue3Names::NameOf(cls, cn, sizeof cn);
                if (saved != live && !strncmp(cn, "SkelControl", 11)) {
                    uint32_t bi[2] = {0, 0};
                    const bool ok = !saved || (!(saved & 7) && (FreeQuarantine::IsPending(reinterpret_cast<const void*>(saved)) ||
                                                                LiveBlockInfo(reinterpret_cast<const void*>(saved), bi)));
                    if (ok) {
                        memcpy(merged.data() + 0x84, &saved, 4);
                        if (++gSkelBindRestores == 1 || gSkelBindRestores % 200 == 0)
                            Logger::Info("SKELBIND: %u SkelControl bindings restored (%u kept live: saved object gone)", gSkelBindRestores,
                                         gSkelBindKept);
                    } else {
                        ++gSkelBindKept;
                    }
                }
            }
            // The skeletal component triple-buffers its bone matrices: the TArrays at comp+0x2F4/+0x300/+0x30C
            // rotate the same three allocations every frame. The headers are kept live (pointers) while the
            // skel_m* contents are restored by address, so after a load whose distance was not a multiple of
            // 3 the matrices sat in the wrong logical slots (synctest36: at the first replayed frame only these
            // three pointers differed, rotated). Same three allocations -> restore the saved rotation.
            if (nf.fightercomp && gBoneRotation && s.size >= 0x2DC) {
                uint32_t sv[3][3], lv[3][3];
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) {
                        memcpy(&sv[a][b], st.bytes[i].data() + 0x2B8 + a * 0xC + b * 4, 4);
                        memcpy(&lv[a][b], reinterpret_cast<const void*>(s.base + 0x2B8 + a * 0xC + b * 4), 4);
                    }
                bool perm = true;
                for (int a = 0; a < 3 && perm; ++a) {
                    bool found = false;
                    for (int b = 0; b < 3; ++b)
                        found |= sv[a][0] && sv[a][0] == lv[b][0] && sv[a][1] == lv[b][1] && sv[a][2] == lv[b][2];
                    perm = found;
                }
                if (perm && (sv[0][0] != lv[0][0] || sv[1][0] != lv[1][0])) {
                    memcpy(merged.data() + 0x2B8, sv, sizeof sv);
                    if (++gBoneRotations == 1 || gBoneRotations == 10 || gBoneRotations == 100 || gBoneRotations == 1000)
                        Logger::Info("BONEROT: restored the saved bone-matrix buffer rotation (%d loads so far)", gBoneRotations);
                }
            }
            if (nf.fightercomp && gCompList && s.size >= 0x378) {
                uint32_t sv[3], lv[3];
                memcpy(sv, st.bytes[i].data() + 0x36C, sizeof sv);
                memcpy(lv, reinterpret_cast<const void*>(s.base + 0x36C), sizeof lv);
                if (sv[0] == lv[0] && sv[2] == lv[2] && sv[1] <= sv[2]) {
                    memcpy(merged.data() + 0x370, &sv[1], 4);  // same block: saved count
                } else if (gListRescue && !lv[0] && sv[0] && sv[1] <= sv[2] && FreeQuarantine::IsPending(reinterpret_cast<const void*>(sv[0]))) {
                    // The list's block was freed after the save (live header null, block still quarantined): restore the
                    // saved header; OnLoad keeps the block allocated because this restored span references it.
                    // animtrace 2026-10-17: the replay saw an empty list where live had one bound element, the PinLimb
                    // went unbound and a script started a different sequence (69 RNG draws, meter +0.056).
                    memcpy(merged.data() + 0x36C, sv, sizeof sv);
                    if (++gListRescued == 1 || gListRescued % 100 == 0) Logger::Info("LISTRESCUE: %u freed bound-anim lists restored", gListRescued);
                } else if (gCompListRebase && sv[1] <= lv[2] && (sv[1] == 0 || (lv[0] && Writable(lv[0], sv[1] * 4)))) {
                    // The array was allocated or grew since the save (the live block differs): the replay still has to
                    // see the saved entries, not the abandoned future's count (BONEHASH +0x3AC off by 1-2 at the first
                    // replayed FightUpdate, synctest104-113). Saved count; saved contents into the live block.
                    const uint8_t* saved = nullptr;
                    for (size_t j = 0; sv[1] && j < st.spans.size(); ++j)
                        if (!strcmp(st.spans[j].name, "complist") && st.spans[j].identity == (uint32_t)(s.base - 0x3C + 0x3A8) &&
                            st.spans[j].size == sv[1] * 4)
                            saved = st.bytes[j].data();
                    if (sv[1] == 0 || saved) {
                        if (saved) memcpy(reinterpret_cast<void*>(lv[0]), saved, sv[1] * 4);
                        memcpy(merged.data() + 0x370, &sv[1], 4);
                        ++gCompListRebased;
                    }
                }
            }            memcpy(reinterpret_cast<void*>(s.base), merged.data(), s.size);
            if (nf.mkactor) NoteMkLoc((uint32_t)s.base - 0x3C, 0, merged.data());
        } else {
            // 'abtest padfull': off restores the AI/remote pad only up to +0x5F8 as before (ring index left live).
            const bool padCut = !gPadFullLoad && s.size > 0x5F8 && nf.aipad;
            if (s.size > 0x5F8 && nf.aipad) {  // how often does the tail past 0x5F8 differ at a load?
                static uint32_t sLoads = 0, sDiff = 0, sIdx = 0;
                ++sLoads;
                const uint8_t* sv = st.bytes[i].data();
                const uint8_t* lv = reinterpret_cast<const uint8_t*>(s.base);
                if (memcmp(sv + 0x5F8, lv + 0x5F8, s.size - 0x5F8)) ++sDiff;
                if (sv[0x5F8] != lv[0x5F8]) {
                    ++sIdx;
                    static int sLogged = 0;
                    if (sLogged++ < 20)
                        Logger::Info("PADTAIL S=%u: +0x5F8 index saved %u live %u (+0x54 saved %u live %u)%s", st.gameFrame, sv[0x5F8], lv[0x5F8],
                                     sv[0x54], lv[0x54], padCut ? " [kept live: padfull off]" : "");
                }
                if (sLoads % 200 == 0) Logger::Info("PADTAIL: %u loads, tail differed %u, ring index differed %u", sLoads, sDiff, sIdx);
            }
            if (nf.movereq) {  // which entry fields change between the save and the load?
                static uint32_t sHist[0x248 / 4], sLoads = 0, sDiffEntries = 0;
                const uint32_t* sv = reinterpret_cast<const uint32_t*>(st.bytes[i].data());
                const uint32_t* lv = reinterpret_cast<const uint32_t*>(s.base);
                bool any = false;
                for (uint32_t w = 0; w < s.size / 4; ++w)
                    if (sv[w] != lv[w]) ++sHist[w], any = true;
                sDiffEntries += any;
                if (++sLoads % 2000 == 0) {
                    char line[400];
                    int ln = 0;
                    for (uint32_t w = 0; w < 0x248 / 4 && ln < 360; ++w)
                        if (sHist[w]) ln = AppendF(line, sizeof line, ln, " +%X:%u", w * 4, sHist[w]);
                    Logger::Info("MOVEREQ: %u entry restores, %u differed; offsets:%s", sLoads, sDiffEntries, ln ? line : " none");
                }
            }
            // The master image with the helper threads (rc23; ~1.3 ms of every load on one core), the rest as before.
            ParallelMem::Copy(reinterpret_cast<void*>(s.base), st.bytes[i].data(), padCut ? 0x5F8 : s.size);
            if (nf.mkactor) NoteMkLoc((uint32_t)s.base - 0x3C, 0, st.bytes[i].data());
        }
        if (padHwKeep) KeepPadDeviceBits(s.base, padHw, st.gameFrame);
        ++restored;
    }
    const int64_t tSpansDone = qpc();
    for (const MapHdr& m : liveMaps) {
        if (!m.obj || m.how == 0 || !Writable(m.obj + 0x138, 0x3C)) continue;
        if (m.how == 1) {
            // A block changed and is not held: header and blocks all stay the abandoned future's.
            memcpy(reinterpret_cast<void*>(m.obj + 0x138), m.bytes, 0x3C);
            static int logged = 0;
            if (logged++ < 10) Logger::Info("NAMEMAP %08X: blocks changed since the save and not held, live header kept", (unsigned)m.obj);
        } else {
            memset(reinterpret_cast<void*>(m.obj + 0x138), 0, 0x3C);  // an empty map: no blocks, refilled lazily
        }
    }
    int emptied = 0;
    for (uintptr_t l : liveExt) {
        bool saved = false;
        for (const Span& s : st.spans) saved |= s.base == l && !strcmp(s.name, "extlist");
        if (saved || !Writable(l, 0x14)) continue;
        uint32_t* h = reinterpret_cast<uint32_t*>(l);
        h[1] = h[2] = h[4] = 0;  // head, tail, pending
        ++emptied;
    }
    if (emptied) Logger::Frame("LOADAB emptied %d external node lists created after the save", emptied);
    // HostFrame is restored too: the script binding cache (MKScriptVM +0x7C/+0x80
    // records) stamps HostFrame, so keeping it live left 24 heap dwords off by
    // the window length (run14). Rollback resimulation re-advances it per tick.
    (void)hostFrame;
    if (gXblockSkipped) {
        std::string names;
        for (auto& kv : gSkipNames) names += " " + kv.first + "=" + std::to_string(kv.second);
        Logger::Info("LOADAB gf=%u: %d xblocks were no longer allocated and were not restored:%s", st.gameFrame, gXblockSkipped, names.c_str());
    }
    // Fighter SkelControl binding state, after every span (it overrides the nodes' kept-live +0xC0).
    if (gBindStateLoad)
        for (int i = 0; i < 2; ++i) {
            if (st.bindRec[i].empty()) continue;
            uint32_t id = 0;
            const uintptr_t obj = CharObj(i, &id);
            if (RestoreBindState(obj ? Read32(obj + 0x10C) : 0, st.bindRec[i])) ++gBindApplied;
            else ++gBindSkipped;
        }
    static int loads = 0;
    if (loads % 300 == 299) Logger::Info("BINDSTATE: binding state restored %u times, skipped %u (a block, count or node changed)", gBindApplied, gBindSkipped);
    if (loads % 300 == 299) Logger::Info("FXACTORS: %u attached-actor spans saved, %u restored at load; %u MK-object actor spans saved, %u restored; %u MK helper blocks saved, %u restored", gFxActorSaved, gFxActorRestored, gMkActorSaved, gMkActorRestored, gMkHelperSaved, gMkHelperRestored);
    if (++loads % 300 == 0)
    {
        Logger::Info("LEARNFIELDS (%s): %d kept fields learned as data over %d loads (%u addresses learned)",
                     gLearnFields == 1 ? "restored" : "report only", gLearnedRestores, loads, (unsigned)gFieldKind.size());
        int shown = 0;
        for (const auto& [k, v] : gLearnedWhere)
            if (shown++ < 30) Logger::Info("LEARNFIELDS   %s: %d (saved value %08X)", k.c_str(), v.first, v.second);
    }
    OnlineKeep::AfterLoad(st.gameFrame, restored >= 0);  // still under heapLocks
    sTailT += qpc() - tSpansDone;
    if (++sProfLoads == 100) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        auto ms = [&](int64_t v) { return v * 1000.0 / f.QuadPart / sProfLoads; };
        std::vector<std::pair<int64_t, std::pair<const char*, uint32_t>>> top;
        for (const auto& [n, v] : sSpanT) top.push_back({v.first, {n, v.second}});
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        char line[700];
        int k = snprintf(line, sizeof line, "LOADAB PROFILE (ms/load, 100 loads): setup %.2f tail %.2f; spans:", ms(sSetupT), ms(sTailT));
        for (size_t j = 0; j < top.size() && j < 10 && k < (int)sizeof line - 60; ++j)
            k = AppendF(line, sizeof line, k, " %s %.2f (%u/load)", top[j].second.first, ms(top[j].first),
                          top[j].second.second / (uint32_t)sProfLoads);
        Logger::Info("%s", line);
        // rc25: setup in parts, and the diff-first merge (per load: spans merged by diff, those with no differing dword, the
        // differing dwords, the live origins classified for them, spans merged whole, 'diffmerge 2' mismatches in total).
        const double nl = sProfLoads;
        Logger::Info("LOADAB PROFILE setup (ms/load): namemaps %.2f extlists %.2f checks+locks %.2f freetree %.2f (%.0f nodes); diffmerge "
                     "mode %d: %.1f spans/load (%.1f unchanged), %.1f dwords differ, %.1f origins classified, %.2f whole-span; %u mismatches",
                     ms(sSetupPart[0]), ms(sSetupPart[1]), ms(sSetupPart[2]), ms(sSetupPart[3]), sFreeNodes / nl, gDiffMerge, gDiffSpans / nl,
                     gDiffUnchanged / nl, gDiffDwords / nl, gDiffOrigins / nl, gDiffFull / nl, gDiffMismatch);
        for (int64_t& v : sSetupPart) v = 0;
        sFreeNodes = 0;
        gDiffSpans = gDiffUnchanged = gDiffDwords = gDiffOrigins = gDiffFull = 0;
        sSpanT.clear();
        sSetupT = sTailT = 0;
        sProfLoads = 0;
    }
    return restored;
}

}  // namespace Snapshot










