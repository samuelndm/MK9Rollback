#include "MKHeap.h"

#include <windows.h>

#include <cstdio>
#include <initializer_list>
#include <utility>

#include "../common/StrAppend.h"
#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "MK9Game.h"

namespace {
uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

uintptr_t FighterObj() {
    const uintptr_t info = MK9Game::Get().playerInfoBase;
    const uintptr_t obj = Read32(info + 0x1C);
    if (!obj || obj == 0xDEADBEEF || Read32(info + 0x20) == 0xFFFFFFFF) return 0;
    return Read32(obj + 4) == Read32(info + 0x20) ? obj : 0;
}

bool AllocationOf(uintptr_t p, MKHeap::Region* out) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    const uintptr_t allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    // Sum the committed, read-write extent of this allocation starting at its base.
    uintptr_t cur = allocBase, end = allocBase;
    while (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) &&
           reinterpret_cast<uintptr_t>(mbi.AllocationBase) == allocBase) {
        if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE)) end = cur + mbi.RegionSize;
        else break;  // stop at the first uncommitted/odd sub-range: only a contiguous image is copied
        cur += mbi.RegionSize;
    }
    out->base = allocBase;
    out->size = (uint32_t)(end - allocBase);
    return out->size > 0;
}
}  // namespace

namespace MKHeap {

// The object heap is a sub-range of the 12 MB master reservation. Its GMalloc
// object stores base at +0x34 and size at +0x38 (runtime-observed 2026-10-14:
// base 0x109EFFC0, size 0x180000; every active pool's free head lies inside).
// Caveat (static analysis 2026-10-15): for heaps of class 0x00C66A3C, [heap+0x34] is the root of the size-keyed
// red-black free tree (ctor 0x0042AAD0 zeroes it, insert 0x0042B0F0, alloc 0x0042B390 walks it), not a base. It
// equals the region start only while the big initial free chunk stays the root. So the last good answer per heap
// is cached and reused while the fighter is still inside it (a moved root made saves fail: "object heap not
// discovered").
bool Discover(Region* out) {
    static uintptr_t cachedHeap = 0;
    static Region cached{};
    const uintptr_t obj = FighterObj();
    const uintptr_t h = Read32(kObjectHeapPtr);
    if (!obj || !h) return false;
    const uintptr_t base = Read32(h + 0x34);
    const uint32_t size = Read32(h + 0x38);
    Region alloc{};
    if (size >= 0x100000 && size <= 0x800000 && obj >= base && obj + 0xC580 <= base + size &&
        AllocationOf(base, &alloc) && base + size <= alloc.base + alloc.size) {
        cachedHeap = h;
        cached = {base, size};
        *out = cached;
        return true;
    }
    if (cachedHeap == h && cached.size && obj >= cached.base && obj + 0xC580 <= cached.base + cached.size &&
        AllocationOf(cached.base, &alloc) && cached.base + cached.size <= alloc.base + alloc.size) {
        *out = cached;
        return true;
    }
    return false;
}

// System segment registry (static analysis 2026-10-15): a binary tree of segment descriptors rooted at [0x00E29308]
// (rw lock 0x00E292F8; lookup 0x00427920: higher addresses at +0x24, lower at +0x28). Descriptor: +0 base, +4 size,
// +0x14 owner heap, +0x18 kind (5 = heap segment, set by 0x00429B00 when a heap grows through [heap+0x18] vt+0x14).
// The owner's +0x24 sums its segment bytes (the "reservation size" MasterRanges used to read).
// Off: runtime (synctest103) showed the master heap is one 12 MB segment from its own parent (0x0F749830), not in this
// registry, and its free-tree root stays at the segment start, so the root-based range was right after all.
bool gRegistryRanges = false;
int HeapSegments(uintptr_t heap, Region* out, int max) {
    uint32_t root = 0;
    if (!CrashLog::SafeRead32(0x00E29308, &root) || !root) return 0;
    uint32_t stack[512];
    int sp = 0, n = 0, visited = 0;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192) {
        const uint32_t d = stack[--sp];
        ++visited;
        uint32_t base = 0, size = 0, owner = 0, kind = 0, hiChild = 0, loChild = 0;
        if (!CrashLog::SafeRead32(d, &base) || !CrashLog::SafeRead32(d + 4, &size) || !CrashLog::SafeRead32(d + 0x14, &owner) ||
            !CrashLog::SafeRead32(d + 0x18, &kind) || !CrashLog::SafeRead32(d + 0x24, &hiChild) || !CrashLog::SafeRead32(d + 0x28, &loChild))
            return -1;  // torn or unreadable tree: caller falls back
        if (owner == heap && kind == 5 && base && size && n < max) out[n++] = {base, size};
        if (hiChild && sp < 512) stack[sp++] = hiChild;
        if (loChild && sp < 512) stack[sp++] = loChild;
    }
    for (int i = 0; i < n; ++i)  // by address
        for (int j = i + 1; j < n; ++j)
            if (out[j].base < out[i].base) std::swap(out[i], out[j]);
    return n;
}

// Master heap [0xED53F0] region, committed pages only. The sub-heaps [0xED53F8], [0xED53FC],
// [0xED5400] used to be left out as "assets", but [0xED5400] had 0x118C0 bytes in use during a
// fight and MK objects free into it (soak4: double free -> free-tree corruption at
// 0x0042AF1D), so they are part of the image now (kExcludeSubHeaps = false). Heap headers
// store base +0x34 and reservation size +0x24 (runtime: master 0xC00000, object heap 0x180000).
constexpr bool kExcludeSubHeaps = false;
int MasterRanges(Region* out, int max) {
    const uintptr_t m = Read32(kMasterHeapPtr);
    if (!m) return 0;
    if (gRegistryRanges) {
        // Every segment the master heap owns, committed read-write pages only. Segments are VirtualAlloc'd
        // (0x0042D490, MEM_COMMIT), so normally the whole segment.
        Region seg[32];
        const int ns = HeapSegments(m, seg, 32);
        if (ns > 0) {
            int n = 0;
            for (int i = 0; i < ns && n < max; ++i) {
                uintptr_t p = seg[i].base;
                const uintptr_t end = seg[i].base + seg[i].size;
                while (p < end && n < max) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
                    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                    const uintptr_t stop = regionEnd < end ? regionEnd : end;
                    if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE)) {
                        if (n > 0 && out[n - 1].base + out[n - 1].size == p) out[n - 1].size += (uint32_t)(stop - p);
                        else out[n++] = {p, (uint32_t)(stop - p)};
                    }
                    p = stop;
                }
            }
            static int lastSegs = 0;
            if (ns != lastSegs) {
                lastSegs = ns;
                char line[400] = "";
                int k = 0;
                for (int i = 0; i < ns && k < 360; ++i) k = AppendF(line, sizeof line, k, " %08X+%X", (unsigned)seg[i].base, seg[i].size);
                Logger::Info("HEAPSEG master heap %08X owns %d segment(s):%s (root-based base %08X)", (unsigned)m, ns, line,
                             Read32(m + 0x34));
            }
            if (n > 0) return n;
        }
    }
    const uintptr_t base = Read32(m + 0x34);
    const uint32_t size = Read32(m + 0x24);  // reservation size (+0x38 is smaller for the master)
    if (!base || size < 0x100000 || size > 0x2000000) return 0;
    Region holes[3];
    int nh = 0;
    for (uintptr_t hp : {0x00ED53F8u, 0x00ED53FCu, 0x00ED5400u}) {
        const uintptr_t h = kExcludeSubHeaps ? Read32(hp) : 0;
        if (!h) continue;
        const uintptr_t hb = Read32(h + 0x34);
        const uint32_t hs = Read32(h + 0x24);
        if (hb >= base && hb + hs <= base + size && hs) holes[nh++] = {hb, hs};
    }
    // sort holes by base
    for (int i = 0; i < nh; ++i)
        for (int j = i + 1; j < nh; ++j)
            if (holes[j].base < holes[i].base) std::swap(holes[i], holes[j]);
    Region coarse[8];
    int nc = 0;
    uintptr_t cur = base;
    for (int i = 0; i < nh && nc < 8; ++i) {
        if (holes[i].base > cur) coarse[nc++] = {cur, (uint32_t)(holes[i].base - cur)};
        cur = holes[i].base + holes[i].size;
    }
    if (cur < base + size && nc < 8) coarse[nc++] = {cur, (uint32_t)(base + size - cur)};
    // Keep committed read-write pages only (the free tail of the reservation is mostly
    // uncommitted; free-block headers live at the start of committed space).
    int n = 0;
    for (int i = 0; i < nc && n < max; ++i) {
        uintptr_t p = coarse[i].base;
        const uintptr_t end = coarse[i].base + coarse[i].size;
        while (p < end && n < max) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            const uintptr_t stop = regionEnd < end ? regionEnd : end;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE)) {
                if (n > 0 && out[n - 1].base + out[n - 1].size == p) out[n - 1].size += (uint32_t)(stop - p);
                else out[n++] = {p, (uint32_t)(stop - p)};
            }
            p = stop;
        }
    }
    return n;
}

void LogInfo() {
    Logger::Info("HEAP master=[%08X]=%08X objheap=[%08X]=%08X idCounter=%08X scriptDepth=%d", kMasterHeapPtr,
                 Read32(kMasterHeapPtr), kObjectHeapPtr, Read32(kObjectHeapPtr), Read32(kObjectIdCounter),
                 (int)Read32(kScriptDepth));
    for (uintptr_t h : {Read32(kMasterHeapPtr), Read32(kObjectHeapPtr)}) {
        if (!h) continue;
        char line[256];
        int n = 0;
        for (int i = 0; i < 0x50; i += 4) n = AppendF(line, sizeof line, n, " %08X", Read32(h + i));
        Logger::Info("HEAP obj %08X:%s", h, line);
    }
    for (int i = 0; i < kPoolCount; ++i) {
        const uintptr_t p = Read32(kPoolPtrs + 4 * i);
        if (!p) continue;
        Region r{};
        const uintptr_t head = Read32(p + 0x30);
        const bool inAlloc = head && AllocationOf(head, &r);
        Logger::Info("POOL %2d %08X vt=%08X free=%08X aba=%08X elem=%X total=%X headAlloc=%08X+%X", i, p, Read32(p),
                     head, Read32(p + 0x34), Read32(p + 0x3C), Read32(p + 0x40), inAlloc ? r.base : 0,
                     inAlloc ? r.size : 0);
    }
    Region r{};
    if (Discover(&r))
        Logger::Info("HEAP object region from fighter: %08X size %X", r.base, r.size);
    else
        Logger::Info("HEAP object region: not discovered (not in a fight?)");
    for (uintptr_t hp : {0x00ED53F8u, 0x00ED53FCu, 0x00ED5400u}) {
        const uintptr_t h = Read32(hp);
        if (!h) continue;
        char line[256];
        int k = 0;
        for (int i = 0; i < 0x50; i += 4) k = AppendF(line, sizeof line, k, " %08X", Read32(h + i));
        Logger::Info("HEAP sub [%08X]=%08X:%s", hp, h, line);
    }
    {  // registry descriptors that belong to (or overlap) the MK heaps: kinds/owners at runtime
        const uintptr_t heaps[5] = {Read32(0x00ED53F0), Read32(0x00ED53F4), Read32(0x00ED53F8), Read32(0x00ED53FC), Read32(0x00ED5400)};
        const uint32_t mroot = heaps[0] ? Read32(heaps[0] + 0x34) : 0;
        uint32_t root = 0;
        CrashLog::SafeRead32(0x00E29308, &root);
        uint32_t stack[512];
        int sp = 0, visited = 0, shown = 0;
        if (root) stack[sp++] = root;
        while (sp > 0 && visited < 8192) {
            const uint32_t d = stack[--sp];
            ++visited;
            uint32_t w[12] = {};
            bool ok = true;
            for (int i = 0; i < 12 && ok; ++i) ok = CrashLog::SafeRead32(d + 4 * i, &w[i]);
            if (!ok) break;
            bool rel = mroot >= w[0] && mroot < w[0] + w[1];
            for (uintptr_t h : heaps) rel |= h && (w[5] == h || (h >= w[0] && h < w[0] + w[1]));
            if (rel && shown++ < 16)
                Logger::Info("HEAPREG desc %08X: base %08X size %X +8 %08X +C %08X +10 %08X +14 %08X +18 %08X +1C %08X", d, w[0], w[1], w[2], w[3],
                             w[4], w[5], w[6], w[7]);
            if (w[9] && sp < 512) stack[sp++] = w[9];
            if (w[10] && sp < 512) stack[sp++] = w[10];
        }
        Logger::Info("HEAPREG %d descriptors walked, %d related to the MK heaps (heaps %08X %08X %08X %08X %08X, master root %08X)", visited,
                     shown, (unsigned)heaps[0], (unsigned)heaps[1], (unsigned)heaps[2], (unsigned)heaps[3], (unsigned)heaps[4], mroot);
    }
    Region mr[8];
    const int n = MasterRanges(mr, 8);
    uint32_t total = 0;
    for (int i = 0; i < n; ++i) {
        Logger::Info("HEAP master range %d: %08X size %X", i, mr[i].base, mr[i].size);
        total += mr[i].size;
    }
    Logger::Info("HEAP master ranges total %X (%u KB)", total, total / 1024);
}

}  // namespace MKHeap



