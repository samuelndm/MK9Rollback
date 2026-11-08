#pragma once
#include <cstdint>

// MKScript real-time natives made deterministic (static analysis, docs/script_scheduling.md):
//  - stopwatches (natives 782-789 -> helpers 0x004931A0..0x004933C0) read the real clocks
//    0x004D5DA0 (ms) / 0x004D5DF0 (us); state tables 0x00E3B6D8 (us) / 0x00E3B7B0 (ms), 0x18 per slot;
//  - native 191 (0x008B7F50, table slot 0x00DF0F3C) returns QPC seconds.
// Real time keeps running while a rollback resimulates frames in a few ms, so a deadline taken
// before S had already passed in the resimulation (a script started 3 frames early). While active
// the six helper call sites and the native slot use a virtual clock that advances 1/60 s per
// GameFrame from an anchor (the real clock at enable / timeline restart). Active from a netplay start to
// the next timeline restart (players), or always with 'lab 1' / 'idlesaves 1'. The tables are in the
// snapshot (Tier A "script_clocks").
namespace ScriptClock {

bool Install();                 // HookManager patches; idempotent
void Anchor(uint32_t gameFrame);  // timeline (re)start or rollback enable
// Netplay start: the same virtual clock on both peers (fixed bases instead of each machine's real clock); activates it.
void AnchorCanonical(uint32_t gameFrame);
void SetActive(bool on);
bool Active();

}  // namespace ScriptClock
