#pragma once
#include <cstdint>

// MK objects reach their UE3 actor through obj+8 (spawned by the MKObj constructor 0x004396E0). A load restores the
// MK heap image, so an object alive at the snapshot comes back holding the actor it had then, and that actor (or one
// of its components) may have been garbage-collected since: the collector ran its destructors (~UObject 0x005F8850
// resets the vtable to UObject's 0x00D46AA0 and clears the GObjObjects slot) and freed it into the quarantine.
//
// Crash (regress r1 2026-11-04 02:06 and 02:50, same scripted inputs, resimulated EngineTick right after a load): the
// dying light effect's destructor 0x0043D130 (vtable 0x00C671D0) read actor+0x1CC (its FXLightComponent), set the
// brightness and called 0x0078F1C0 -> FComponentReattachContext 0x0078BF50 -> component vt+0xE8, past the end of the
// UObject vtable: eip = the float 0x3C40C0C1 stored after it.
//
// Two checks, both no-ops in normal play (a collected object is never reachable there):
//   - 0x0043D176, the light destructor's component read: a collected actor or component reads as none (no brightness
//     change, no reattach; render state only).
//   - 0x00439A36, the MKObj base destructor's obj+8 read: a collected actor reads as none, so the destructor skips the
//     un-root, the DestroyActor call and the back-reference reset at actor+0x9C (writes into freed memory).
namespace ActorGuard {

bool Install();
uint32_t Hits();  // collected actors or components skipped

}  // namespace ActorGuard
