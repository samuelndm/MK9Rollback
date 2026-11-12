#pragma once

// Milestone 11, rumble: MKKE imports XInputSetState from XINPUT1_3.dll by ordinal and calls it through
// its import table entry (0x00C5581C, read-only .rdata). That pointer is redirected to a wrapper
// that drops calls made while a rollback resimulates frames and, once the resimulation ends, sends the
// last motor state requested during it (rumble is a level, not an event: the last state wins).
namespace Rumble {

bool Install();       // finds and redirects the pointer; false if xinput is not loaded or not found
void EndResim();      // flush the last state requested during the resimulation
void Uninstall();     // MK9R_Unload: the pointer is the game's own again

}  // namespace Rumble
