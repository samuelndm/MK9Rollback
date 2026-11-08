#pragma once
#include <cstdint>

// Build-pinned knowledge about MKKE.exe. Every address below was derived from
// static analysis of exactly one executable (see docs/build_identity.md) and is
// re-located at runtime with a byte signature; the two must agree or we refuse
// to install anything (fail closed).
namespace MK9Game {

constexpr const char* kExpectedSha256 = "D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6";
constexpr uintptr_t kPreferredBase = 0x00400000;

struct Addresses {
    uintptr_t engineTick;          // 0x0080F670  thiscall(engine, float dt), RET 4  (engine vtable +0xC0)
    uintptr_t engineVtableSlot;    // 0x00D60DF0  vtable entry holding engineTick
    uintptr_t fightUpdatePlayers;  // 0x008ACD90  cdecl void(void)
    uintptr_t fightUpdateCallSite; // 0x0080F70B  E8 call inside engineTick (only caller)
    uintptr_t gameInfoPtr;         // 0x00DEF4B4  holds FG_GAME_INFO* (expected 0x00E89B70)
    uintptr_t pauseFlag;           // 0x00E43054  int, nonzero while paused (getter 0x00406790)
    uintptr_t playerInfoBase;      // 0x00E89D7C  PlayerInfo[4], stride 0xC8C (from GetInfo 0x0080E540)
};

constexpr uint32_t kPlayerInfoStride = 0xC8C;
constexpr uint32_t kGameInfoHostFrame = 0x6C;  // +1 every main-loop iteration
constexpr uint32_t kGameInfoGameFrame = 0x70;  // +1 per iteration only when unpaused

// Verifies the host executable hash and resolves all addresses. Logs every
// step. Returns false (and installs nothing) on any mismatch.
bool VerifyBuildAndResolve(Addresses* out);

const Addresses& Get();

}  // namespace MK9Game
