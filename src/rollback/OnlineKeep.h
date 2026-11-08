#pragma once
#include <cstdint>

#include "Snapshot.h"

// MK9's own online connection across rollback loads. The connection object (0x11AA7710 in the 2026-11-01 19:08 log) and
// the rest of MK9's online library live in MK heap #5 ([0x00ED5400], 1 MB), which the master image restores like every
// MK heap (MKHeap::MasterRanges, kExcludeSubHeaps = false since soak4). So every load rewound the connection to the
// snapshot frame: its sequence state, its last-packet times (MK9's packet gaps after each load, rollback only, 19:11),
// while the real packets of those frames had already been sent and received.
//
// Keeping heap #5's memory as it is (modes 2/3) is sound for the heap's allocator when:
// - nothing was allocated or freed in heap #5 since the save (FreeQuarantine::Heap5Ops, counted at the MK heap hooks),
//   so the heap's blocks and free tree are the same in the snapshot and now;
// - heap #5's header equals the saved one (its spinlock aside): also catches a reset (vt+0xC) the hooks do not see;
// - the memory lies in the master image, and (online) contains MK9's connection.
// Snapshot spans inside it other than the master image itself are restored as saved. The copy and the write-back run
// inside LoadAB while it holds the MK heaps' spinlocks.
// Not proven for the heap's contents: MK fight objects were seen in heap #5 too (soak4), and keeping one of them live
// would desynchronize the replays (review 2026-11-01). So the default only measures: on a sample of the loads it logs
// what the restore changed there, inside MK9's connection and outside it.
// 'onlinekeep 0|1|2|3': off, measure in online sessions (default), keep live in online sessions, keep live in every
// session (lab: SyncTest it offline).
namespace OnlineKeep {

void BeforeSave();                                // right before Snapshot::SaveAB
void OnSave(uint32_t gf);                         // after a successful save (game thread)
bool BeforeLoad(const Snapshot::StateAB& snap);   // LoadAB, heap locks held, before any write: true when it will keep
void AfterLoad(uint32_t S, bool restored);        // LoadAB, before it returns: measures, and puts the live bytes back
void OnSessionEnd();                              // session summary (logged when a load was seen)
void SetMode(int mode);
int Mode();

}  // namespace OnlineKeep
