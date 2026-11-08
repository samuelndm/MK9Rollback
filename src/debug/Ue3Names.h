#pragma once
#include <cstddef>
#include <cstdint>

// UE3 object identification for diagnostics. UObject layout (runtime-verified 2026-10-14):
// +0x4 index, +0x24 Outer, +0x28 Name (FName index, +0x2C number), +0x30 Class.
// GNames: data [0x00E7C058], count [0x00E7C05C], entry +0x10 = ANSI string (0x005F51D0).
namespace Ue3Names {

// "Name_N" of a UObject, or "?" if it does not look like one.
void NameOf(uint32_t obj, char* out, size_t n);
// Nearest UObject header at or below addr (within maxBack bytes): vtable in the image and a
// Class whose own name resolves. Returns 0 if none.
uint32_t FindObjectBack(uint32_t addr, uint32_t maxBack);
// "Class:Name+0xOff" for an arbitrary address (or "?").
void Describe(uint32_t addr, char* out, size_t n);

}  // namespace Ue3Names
