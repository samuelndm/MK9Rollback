#pragma once
#include <cstdint>

// Read-only fighter state. Offsets: MK9Hook (verified against this build in
// docs/mk9hook_findings.md) + GetObj 0x00895B70 handle validation logic.
namespace Fighter {

struct State {
    bool valid;
    uint32_t obj;      // character object (PlayerInfo+0x1C, generation-checked by +0x20)
    float life;        // obj+0x78C0
    float pos[3];      // [obj+0x08]+0xDC (actor location)
};

State Read(int player);

}  // namespace Fighter
