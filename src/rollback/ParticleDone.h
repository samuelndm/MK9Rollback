#pragma once
#include <cstdint>

// MK particles die when their UE3 ParticleSystemComponent reports completion: 0x00436690 reads the component
// ([obj+0xF8]) flags at +0x2C0 and deletes the object when bit 2 (bWasCompleted) is set. The component is engine state
// outside every snapshot, so after a load it still says "completed" from the abandoned future and the replay deleted
// the particle frames early. Its freed slot then went to another object, and MK gameplay diverged: the call-#0 group
// of SyncTest residuals (soak 9807: CachedParticle id 0x10000B8F deleted in the replay's first frame only, killer
// 0x008AE986 <- 0x004366BF).
//
// The read at 0x004366A6 goes through this module. The first frame a component is seen completed (live timeline) is
// remembered per component and MK id; a replay behind that frame reads the bit as clear, so the particle dies on the
// same frame as in the original timeline.
namespace ParticleDone {

bool Install();
void Clear();  // timeline restart / renumbering: frame stamps of the old timeline are meaningless
uint32_t Held();  // reads answered "not completed yet" (replays)

}  // namespace ParticleDone
