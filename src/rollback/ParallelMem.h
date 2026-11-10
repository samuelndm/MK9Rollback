#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Big memory passes of a save or a load split between the game thread and up to 3 helper threads (rc23, 2026-11-09).
// Parallel restore is what NRS described for Injustice 2's rollback (GDC 2018, "8 Frames in 16ms"). Each pass is cut in
// 128 KB chunks handed out through one counter; the game thread takes chunks too and returns only when every chunk is
// done, so the result is the single-threaded pass's and a helper that wakes late never makes it slower.
// Game thread only; a call from another thread (or while a pass runs) does the pass alone.
namespace ParallelMem {

// 'memthreads N': helpers per pass (0: the game thread alone, as before rc23; -1: from the processor count).
void SetHelpers(int n);
int LastHelpers();  // helpers the last pass used
void Stop();        // MK9R_Unload: the helpers leave this module's code

// memcpy; below 256 KB the caller copies alone.
void Copy(void* dst, const void* src, size_t bytes);
// A copy INTO a snapshot slot (saves only; rc25). The same bytes as Copy, but from 64 KB up the stores bypass the cache
// (SSE2 non-temporal stores, each thread's sfence before its chunk counts as done): a slot is read again only by a load,
// so the ~12 MB a save writes neither reads the cold destination first nor evicts the game's working set from the cache
// before the next tick. Loads into game memory keep Copy (normal stores: the replay's tick finds the data cached).
void CopyToSlot(void* dst, const void* src, size_t bytes);
void SetSlotStreaming(bool on);  // 'slotnt 0|1': CopyToSlot's non-temporal stores on (default) or plain memcpy

// Every aligned dword in 'ranges' {base, size} inside [lo, hi] that is in the open-addressed set 'set' (cap a power of
// two, 0 = empty slot, hashed as (v >> 3) * 2654435761) is appended to 'hits' (in no particular order, maybe repeated).
void ScanForValues(const std::vector<std::pair<uintptr_t, uint32_t>>& ranges, const uint32_t* set, uint32_t cap, uint32_t lo,
                   uint32_t hi, std::vector<uint32_t>& hits);

}  // namespace ParallelMem
