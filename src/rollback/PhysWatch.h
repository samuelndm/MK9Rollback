#pragma once
#include <cstdint>

// Rigid-body (PhysX) lifetime events inside the rollback window.
//
// PhysX scene state lives outside the game's memory model (external module) and cannot be
// snapshotted. soak8 crashed in the PhysX module during a resimulated frame: 0x006D2900
// (thiscall component; terminates its per-body physics: entries +0x12C handle / +0x124 index,
// then 0x0074FB20 processes the scene's pending-release list) released bodies that the
// restored state no longer matched. This module records such events per GameFrame so a
// rollback across them can be refused.
namespace PhysWatch {

bool Install();
void SetFrame(uint32_t gf);
// True (with a reason) if a rigid-body termination happened at gf >= S, or an actor destroyed at
// gf >= S is referenced by the snapshot (referenced == nullptr: every destroy counts). A destroyed
// actor that restored state does not point at (LifeSpan expiry 0x007AEC5C, cinematic spawns
// 0x004BB857: never MK-owned, docs/load_refusals.md) stays destroyed without harm.
bool EventsSince(uint32_t S, char* why, size_t whyLen, bool (*referenced)(uint32_t actor) = nullptr);
void Trim(uint32_t oldestNeededGf);
void Clear();
// After a load to S: events at gf >= S belong to the abandoned timeline.
void DropFrom(uint32_t S);

}  // namespace PhysWatch
