#pragma once
#include <cstdint>

// The HUD's handle lookup 0x00B8FFD0 (33 callers in 0x0083xxxx-0x0084xxxx): handle -> table [0x00FE2004] (+0x44
// entries, +0x48 count) -> entry->+0xA4, or 0 for an invalid handle. The table belongs to a library outside every
// snapshot. A handle restored with the MK heap could name a slot the abandoned future had already emptied, and the
// lookup dereferenced the null entry: crash 0x00B9000C in a resimulated EngineTick (seen once earlier, then soak
// 11001 gf 2224, caller 0x008433EB with the HUD manager in esi). The function is replaced by the same lookup with a null
// check: an emptied slot answers 0 like any other invalid handle, which every caller already handles.
// The releases of the +0x44 table (0x00B91480) and the +0x5C table (0x00B90070) dereferenced the entry the same way:
// crash 0x00B914C1 in a resimulated EngineTick (soak 11105, 2026-10-25, HUD manager in edi, after two lookup hits).
// They are replaced by the same checks plus a null test; an emptied slot is already released, so nothing is done.
// The +0x38 and +0x50 releases (0x00B90C80, 0x00B91690) and the raw lookups 0x00B8FF80/0x00B90020 test null already.
namespace HandleGuard {

// Also (rc11): the fighter IK list walk 0x0077D567 skips an element with no bound object and no node (crash 0x0077D56A),
// and the render data delete command 0x007B0A90 is dropped when a load put the data it deletes back at its component's
// +0x268 (use after free 0x006F2D4D / 0x007145BD). rc12: a cinematic action's object variable naming a dead object is
// taken as none at 0x007A0B7E (crash 0x007A0BAC). See HandleGuard.cpp.
bool Install();
uint32_t NullHits();  // lookups and releases that found an emptied slot, IK list entries skipped
uint32_t IkNodeSkips();
uint32_t RenderDataKept();          // render data delete commands dropped
void SetRenderDataGuard(bool on);  // default on

}  // namespace HandleGuard
