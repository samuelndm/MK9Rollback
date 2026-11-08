#pragma once
#include <windows.h>

#include <cstdint>

// VirtualQuery with a small region cache, for the per-frame snapshot paths (save profile
// 2026-10-15: VirtualQuery syscalls were ~1.5 ms of a 3.4 ms save). Regions do not change
// within one save or load: callers bracket each with Begin()/End(). Outside a bracket queries
// are not cached (a stale region could be stale by a whole frame). Game thread only.
namespace MemQuery {

struct Entry {
    uintptr_t lo, hi;
    DWORD state, protect, type;
};
inline Entry gCache[64];
inline int gCount = 0, gNext = 0;
inline bool gOn = false;
// Off by default: with the cache on, Gate E runs corrupted MK/GMalloc heap free trees (bisect 2026-10-15:
// cache off 0 crashes in 106 rollbacks, on 1 crash in 51; runs 5-7 all crashed). Suspected: region info
// going stale while worker threads commit/free memory during a save. Command 'memcache 0|1'.
inline bool gEnabled = false;
inline DWORD gOwner = 0;
inline uint32_t gVqCalls = 0;  // VirtualQuery calls made through Query (diagnostic: save profile; racy across threads)

inline void Begin() {
    gCount = gNext = 0;
    gOwner = GetCurrentThreadId();
    gOn = gEnabled;
}
inline void End() { gOn = false; }
struct Scope {
    Scope() { Begin(); }
    ~Scope() { End(); }
};
// The cache on regardless of 'memcache', for a stretch where the heaps are locked (LoadAB's span loop holds every MK
// heap's and GMalloc's lock: no other thread can commit or release heap memory there). Its ~2000 spans asked VirtualQuery
// at least once each, several microseconds per call in a process this size: ~60% of LoadAB's 12 ms (LOADAB PROFILE
// 2026-11-04). Only committed regions are cached, as always.
struct LockedScope {
    bool was;
    LockedScope() : was(gOn) {
        gCount = gNext = 0;
        gOwner = GetCurrentThreadId();
        gOn = true;
    }
    ~LockedScope() {
        gOn = was;
        gCount = gNext = 0;
    }
};

inline bool Query(uintptr_t p, Entry& out) {
    const bool cached = gOn && GetCurrentThreadId() == gOwner;
    for (int i = 0; cached && i < gCount; ++i)
        if (p >= gCache[i].lo && p < gCache[i].hi) {
            out = gCache[i];
            return true;
        }
    MEMORY_BASIC_INFORMATION mbi;
    ++gVqCalls;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    out = {lo, lo + mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type};
    // Only committed regions are cached: a reserved region can be committed by another thread
    // moments later (heap growth), and a stale "not committed" made pointers into the new pages
    // look like data in the snapshot's pointer test (restored by value -> stale heap pointers).
    if (!cached || mbi.State != MEM_COMMIT) return true;
    gCache[gNext] = out;
    gNext = (gNext + 1) % 64;
    if (gCount < 64) ++gCount;
    return true;
}

// [p, p+n) committed, readable (not guard / no-access) and inside one region.
inline bool Readable(uintptr_t p, size_t n) {
    Entry e;
    return p >= 0x10000 && Query(p, e) && e.state == MEM_COMMIT && !(e.protect & (PAGE_GUARD | PAGE_NOACCESS)) && p + n <= e.hi;
}
// Same, and writable.
inline bool Writable(uintptr_t p, size_t n) {
    Entry e;
    return p >= 0x10000 && Query(p, e) && e.state == MEM_COMMIT && (e.protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
           p + n <= e.hi;
}

}  // namespace MemQuery
