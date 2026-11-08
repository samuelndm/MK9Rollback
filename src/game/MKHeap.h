#pragma once
#include <cstdint>

// The MK gameplay object heap (docs/state_ownership.md §2): GMalloc sub-heap
// object at [0xED53F4] (1.5 MB region, recreated per map load) holding the
// fixed-size pools for fighters, script VMs, scripts and generic MK objects.
namespace MKHeap {

constexpr uintptr_t kMasterHeapPtr = 0x00ED53F0;
constexpr uintptr_t kObjectHeapPtr = 0x00ED53F4;
constexpr uintptr_t kPoolPtrs = 0x00ED5404;  // 13 pool object pointers (0x48 bytes each)
constexpr int kPoolCount = 13;
constexpr uintptr_t kObjectIdCounter = 0x00DE820C;
constexpr uintptr_t kScriptDepth = 0x00DE8628;  // -1 when the script VM is idle

struct Region {
    uintptr_t base;
    uint32_t size;
};

// Locates the object heap's backing reservation from a fighter object address
// (fighters are allocated from its MKFGCharacterObj pool). Returns false if not
// in a fight or if the region looks wrong.
bool Discover(Region* out);

// Logs heap/pool objects, the discovered region and the guard globals.
void LogInfo();

// The master heap's region with the asset/file-loader sub-heaps cut out (they hold
// loaded data, immutable during a fight). Contains the object heap, pool overflow and the
// master's own in-region free tree. Returns the number of ranges written.
int MasterRanges(Region* out, int max);

// The segments the system segment registry records for this heap (kind 5), by address. -1: the registry was unreadable.
int HeapSegments(uintptr_t heap, Region* out, int max);

}  // namespace MKHeap
