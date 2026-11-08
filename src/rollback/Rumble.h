#pragma once

// Milestone 11, rumble: MKKE loads xinput1_3 at runtime and calls XInputSetState through a pointer
// it got from GetProcAddress (a global in its image). That pointer is redirected to a wrapper that
// drops calls made while a rollback resimulates frames and, once the resimulation ends, sends the
// last motor state requested during it (rumble is a level, not an event: the last state wins).
namespace Rumble {

bool Install();       // finds and redirects the pointer; false if xinput is not loaded or not found
void EndResim();      // flush the last state requested during the resimulation
void Uninstall();

}  // namespace Rumble
