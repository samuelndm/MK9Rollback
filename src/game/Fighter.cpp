#include "Fighter.h"

#include <windows.h>

#include <cstring>

#include "MK9Game.h"

namespace {
uintptr_t gValidatedObj[4] = {};
uintptr_t gValidatedActor[4] = {};

bool Readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (p < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }
}  // namespace

namespace Fighter {

State Read(int player) {
    State s{};
    if (player < 0 || player > 3) return s;
    const uintptr_t info = MK9Game::Get().playerInfoBase + player * MK9Game::kPlayerInfoStride;
    // Same checks as GetObj (0x00895B70): non-null, not 0xDEADBEEF, id != -1, obj+4 == id.
    const uintptr_t obj = Read32(info + 0x1C);
    const uint32_t id = Read32(info + 0x20);
    if (obj == 0 || obj == 0xDEADBEEF || id == 0xFFFFFFFF) return s;
    if (gValidatedObj[player] != obj) {
        if (!Readable(obj, 0x78C8)) return s;
        gValidatedObj[player] = obj;
    }
    if (Read32(obj + 4) != id) return s;
    s.obj = (uint32_t)obj;
    memcpy(&s.life, reinterpret_cast<const void*>(obj + 0x78C0), 4);
    const uintptr_t actor = Read32(obj + 0x08);
    if (actor && gValidatedActor[player] != actor) {
        if (!Readable(actor, 0xE8)) return s;
        gValidatedActor[player] = actor;
    }
    if (actor) memcpy(s.pos, reinterpret_cast<const void*>(actor + 0xDC), 12);
    s.valid = true;
    return s;
}

}  // namespace Fighter
