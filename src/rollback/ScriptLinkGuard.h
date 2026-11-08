#pragma once
#include <cstdint>

// The script module link 0x0045CE90 runs once per module (flag module+0x5C): for each import entry in module+0x94
// (count +0x28) it turns the name offset at entry+4 into a pointer (entry+4 += [module+0xA8] - 1, at 0x0045CEF9),
// looks the name up (0x0045B0F0, _stricmp) and resolves it, then applies absolute fixups and sets the flag.
//
// The module object lives in the restored master heap image, its import table in an MK sub-heap outside it (heap #2
// at 0x117AFFC0, below the mkmaster span). A load to a frame before the link put the flag back to 0 while the entries
// kept their pointers, the link ran again and added the base twice: crash in _stricmp reading 0x22F6621B (about twice
// the table address) from 0x0045CF20 -> 0x0045B168 (load1_B 2026-11-04, a 'netplayload 1' session in the intro).
//
// The add is made idempotent: an entry whose +4 still holds the pointer this guard produced for it is left alone. A
// fresh table (a real first link, or memory reused by another module) holds a small offset, never that pointer.
// The other fixups write absolute values and repeat safely.
namespace ScriptLinkGuard {

bool Install();
uint32_t Skips();  // relocations skipped because the entry was already relocated

}  // namespace ScriptLinkGuard
