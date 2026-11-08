#pragma once
#include <cstdint>
#include <utility>
#include <vector>

// Rollback-safe general-allocator frees.
//
// MK objects in the restored heap image own blocks in UE3's general allocator
// (GMalloc at [0x00E3EFC8]: e.g. MK anim-node arrays at +8, collision shapes).
// If such an object dies after a snapshot, its blocks are freed and reused; a load
// then resurrects the object pointing at reused memory (run31 crash: EIP in heap
// after a resimulated anim update). While rollback mode is active, frees issued
// during simulation phases (world tick, engine tick, resimulation; any thread) are
// quarantined and only performed once no loadable snapshot predates them.
//
// Hooked: outer GMalloc vtable 0x00C5ABB4 +0x10 (0x004056A0, ret 4) and the inner
// allocator's vtable 0x00C5AB58 +0x18 (0x00405AF0; Realloc frees the old block through it).
// (A first version hooked inner +0x10, which is Malloc: crashed the game, run33.)
namespace FreeQuarantine {

bool Install();
void Uninstall();               // releases everything still quarantined (game thread)
void SetActive(bool on);
// Defer frees that reach a non-image MK-heap instance directly (GMalloc's medium heap via appFree).
void SetHeapDefer(bool on);
void SetUnfreeAll(bool on);  // load: every block freed after S is live again (old behaviour; leaks non-MK frees)
void SetFrame(uint32_t gf);     // current GameFrame stamp for new entries
void Release(uint32_t oldestNeededGf);  // free entries stamped < oldestNeededGf
void ReleaseAll();
// A load to snapshot frame S: frees stamped >= S happened in the abandoned timeline, so
// those blocks are live again (the resimulation re-frees them if it still does). Returns count.
// Older blocks freed after S by non-MK code are requeued for release instead, unless a dword in the restored
// ranges {base, size} (scanned live, after the restore) still points at them.
int OnLoad(uint32_t S, const std::vector<std::pair<uintptr_t, uint32_t>>* restored = nullptr);
// After restores that run later in the same load (CinemaRestore writes its objects and arrays back after OnLoad): the
// blocks OnLoad requeued that 'ranges' (live, after that restore) still point at are live again, logged with 'who'.
// Combo soak 18 (2026-11-07): the garbage collector read an object array whose block the quarantine had released.
int ProtectReferenced(uint32_t S, const std::vector<std::pair<uintptr_t, uint32_t>>& ranges, const char* who);
void SetProtectLate(bool on);  // 'protectlate 0|1' (default 1)
// Blocks freed during a load after OnLoad's scan (render-thread commands of the abandoned timeline) are offered to
// ProtectReferenced too; this counts those it made live again.
uint32_t LateProtected();
uint32_t Pending();
bool IsPending(const void* p);  // p is a block the game freed and the quarantine still holds (dead to the game)
// Load to frame S: p is a block the game had already freed before S and the quarantine still holds, so a pointer to
// it restored from S's save was dangling at the save. Blocks freed at or after S are not (the load made
// them live again, or ProtectReferenced does). Only blocks still held count: a released one may have been handed out
// again before the save. *freedGf (optional) gets the free's stamp.
bool FreedBefore(const void* p, uint32_t S, uint32_t* freedGf = nullptr);
uint64_t PendingBytesApprox();
void LogCensus();  // diagnostic: what is held (parked, future-stamped, recent, kinds, top caller)
// Blocks whose contents ring slot 'slot' restores (frees of these are deferred on any thread).
void SetRestoreBlocks(int slot, const std::vector<uint32_t>& blocks);
void ClearRestoreBlocks();
// 0: defer game-thread/in-tick frees + restored blocks; 1: restored blocks only (default).
void SetMode(int mode);
void LogTopCallers(int n);  // diagnostic: pending entries grouped by free caller
// Diagnostic: was a block starting within span bytes below p really freed by the quarantine recently?
// block (optional) receives the start of the matching block: with span > 1 it can be a neighbour of p.
// minReleasedGf (optional): stop at the first entry released before that frame (the ring is in release order, non-
// decreasing frames within a timeline); LoadAB asks only about releases after the loaded frame. With it, entries from
// before the current timeline (NewTimeline) are never matched: their frame numbers are the old timeline's.
bool RecentlyReleased(const void* p, uint32_t span, uint32_t* freedGf, uint32_t* releasedGf, uint32_t* caller,
                      uint32_t* block = nullptr, uint32_t minReleasedGf = 0);
// A timeline restart or the netplay renumbering (after ReleaseAll): releases so far belong to the old frame numbers.
void NewTimeline();
// Allocations + frees so far in MK heap #5 ([0x00ED5400], MK9's online library); false while they are not counted
// (the heap hooks are not installed).
bool Heap5Ops(uint32_t* ops);

}  // namespace FreeQuarantine
