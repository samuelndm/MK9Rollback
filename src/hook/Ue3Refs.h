#pragma once
#include <cstdint>
#include <utility>
#include <vector>

// UE3 objects that hold raw pointers to MK heap objects (outside the snapshot).
//
// Class A (UE3 InternalConstructor 0x005DA820, size 0xCC, vtable 0x00D3EAC0, interface
// sub-vtable at +0xB0): a singly linked list of nodes {MK object +0, next +4}, head at +0xB4,
// count at +0xBC. Teardown (vtable slot 10, 0x00762390) loops "while (count)
// head->value->vt[1](1)"; each MK object's destructor unlinks itself (0x00765F40). After a
// load that MK object may no longer exist (run41 crash: EIP=-1 from a freed pool slot).
//
// Instances are recorded by hooking the InternalConstructor. Their holder cells are
// saved with every snapshot and, after a load, any cell that does not name a valid live
// MK object is cleared (the destroy path skips a null pointer).
namespace Ue3Refs {

bool Install();
void Uninstall();
// (address, size) cells to include in a snapshot.
void CollectCells(std::vector<std::pair<uintptr_t, uint32_t>>& out);
// After a load to snapshot frame S: zero the count of instances created at/after S and
// unlink nodes that do not name valid live MK objects. Returns instances fixed.
int FixDangling(uint32_t S);
uint32_t TrackedCount();
// True if a class-A instance that existed at frame S was torn down since (load must refuse:
// restored MK objects would point at a destroyed owner; run53 crash in 0x00761411).
bool TornDownSince(uint32_t S, char* why, size_t whyLen);
// GameFrame was reset/jumped: existing instances predate every future snapshot.
void OnTimelineRestart();

}  // namespace Ue3Refs





