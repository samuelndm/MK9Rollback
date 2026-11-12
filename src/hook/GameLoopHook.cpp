#include "GameLoopHook.h"

#include <windows.h>

#include <cstring>

#include "../common/StrAppend.h"
#include "../debug/CommandChannel.h"
#include "../debug/CrashLog.h"
#include "../debug/Lab.h"
#include "../debug/Logger.h"
#include "../game/Fighter.h"
#include "../game/Input.h"
#include "../game/MK9Game.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/Snapshot.h"
#include "../rollback/StateHash.h"
#include "HookManager.h"
#include "../debug/AllocWatch.h"
#include "../debug/AnimInputTrace.h"
#include "ObjectHooks.h"
#include "../rollback/FpuGuard.h"
#include "../rollback/FramePace.h"
#include "../rollback/RngTrace.h"
#include "../rollback/ScriptRng.h"

#include <cstdio>

namespace {
using EngineTickFn = void(__thiscall*)(void* engine, float dt);
using FightUpdateFn = void(__cdecl*)();

EngineTickFn oEngineTick = nullptr;
FightUpdateFn oFightUpdate = nullptr;

volatile LONG gInFlight = 0;
uint32_t gEngineTicks = 0;          // engineTick calls since install
uint32_t gIdleTicks = 0;            // consecutive engine ticks without a fight update
bool gFightedThisTick = false;
bool gCombatActive = false;         // last state reported to the main log
uint32_t gLastGameFrame = 0;
uint32_t gSkippedCalls = 0;         // FightUpdatePlayers calls whose body is gated off
uint32_t gLastSkipKey = ~0u;
float gLastDt = 0;

struct InFlightGuard {
    InFlightGuard() { InterlockedIncrement(&gInFlight); }
    ~InFlightGuard() { InterlockedDecrement(&gInFlight); }
};

uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

uint32_t GameInfoField(uint32_t offset) {
    const uintptr_t gi = Read32(MK9Game::Get().gameInfoPtr);
    return gi ? Read32(gi + offset) : 0;
}

uint32_t FloatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Mirrors the early-out at the top of FightUpdatePlayers (0x008ACD90..0x008ACDD6):
//   IsFightMode() [0x00887AF0: GameInfo+0x8 mode in {0-3,5-7,10-14}]
//   && (flags10 & 1 || (GameInfo+0x4 == 0xB && !(flags10 & 0x400)))
//   && !(flags14 & 0x10)
bool FightBodyRuns(uint32_t* mode, uint32_t* subMode, uint32_t* flags10, uint32_t* flags14) {
    *mode = GameInfoField(0x8);
    *subMode = GameInfoField(0x4);
    *flags10 = GameInfoField(0x10);
    *flags14 = GameInfoField(0x14);
    static const uint8_t kFightMode[15] = {1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1};
    const bool fightMode = *mode <= 14 && kFightMode[*mode];
    const bool active = (*flags10 & 1) || (*subMode == 0xB && !(*flags10 & 0x400));
    return fightMode && active && !(*flags14 & 0x10);
}

void LogFightFrame() {
    uint32_t mode, subMode, flags10, flags14;
    const bool body = FightBodyRuns(&mode, &subMode, &flags10, &flags14);
    if (!body) {
        ++gSkippedCalls;
        // Menus/attract: log state changes only, not every frame.
        const uint32_t key = mode ^ (subMode << 8) ^ (flags10 << 16) ^ flags14;
        if (key != gLastSkipKey) {
            Logger::Frame("SKIP et=%u mode=%u sub=%u f10=%08X f14=%08X (fight body gated off)", gEngineTicks, mode,
                          subMode, flags10, flags14);
            gLastSkipKey = key;
        }
        return;
    }
    gLastSkipKey = ~0u;
    ++GameLoopHook::gSimulationFrame;

    const uint32_t hostFrame = GameInfoField(MK9Game::kGameInfoHostFrame);
    const uint32_t gameFrame = GameInfoField(MK9Game::kGameInfoGameFrame);
    const uint32_t paused = Read32(MK9Game::Get().pauseFlag);
    const Input::PadSnapshot p1 = Input::ReadPad(0);
    const Input::PadSnapshot p2 = Input::ReadPad(1);
    const Fighter::State f1 = Fighter::Read(0);
    const Fighter::State f2 = Fighter::Read(1);

    // Gate check for the trace: the game frame should advance by exactly 1 per fight update.
    const char* step = (GameLoopHook::gSimulationFrame == 1 || gameFrame == gLastGameFrame + 1) ? "" : " GF_JUMP";
    gLastGameFrame = gameFrame;

    Logger::Frame(
        "FRAME%s %llu gf=%u hf=%u pause=%u et=%u mode=%u sub=%u f10=%08X f14=%08X dt=%08X%s | "
        "P1 v=%d pad=%08X i=%d f=%08X cur=%08X,%08X,%08X prev=%08X rings=%u/%u/%u lock=%u | "
        "P2 v=%d pad=%08X i=%d f=%08X cur=%08X,%08X,%08X prev=%08X rings=%u/%u/%u lock=%u",
        Rollback::gIsResimulating ? "R" : "", (unsigned long long)GameLoopHook::gSimulationFrame, gameFrame, hostFrame,
        paused, gEngineTicks, mode,
        subMode, flags10, flags14, FloatBits(gLastDt), step, p1.valid, p1.pad, p1.index, p1.flags, p1.cur[0], p1.cur[1], p1.cur[2], p1.prev[0],
        p1.ringIndex[0], p1.ringIndex[1], p1.ringIndex[2], p1.inputLock, p2.valid, p2.pad, p2.index, p2.flags,
        p2.cur[0], p2.cur[1], p2.cur[2], p2.prev[0], p2.ringIndex[0], p2.ringIndex[1], p2.ringIndex[2],
        p2.inputLock);
    // The state and per-region hash lines (lab: ~1.7 MB hashed per fight frame, replays included); the FRAME line stays
    // (the two-instance tests read it to see a fight running).
    if (!Lab::On()) {
        CommandChannel::OnFightFrame(gameFrame);
        return;
    }
    Logger::Frame(
        "  STATE %llu ovr=%d%d | F1 v=%d life=%.4f pos=%.3f,%.3f,%.3f | F2 v=%d life=%.4f pos=%.3f,%.3f,%.3f | "
        "bits=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X",
        (unsigned long long)GameLoopHook::gSimulationFrame, Input::IsOverridden(0), Input::IsOverridden(1), f1.valid,
        f1.life, f1.pos[0], f1.pos[1], f1.pos[2], f2.valid, f2.life, f2.pos[0], f2.pos[1], f2.pos[2],
        FloatBits(f1.life), FloatBits(f1.pos[0]), FloatBits(f1.pos[1]), FloatBits(f1.pos[2]), FloatBits(f2.life),
        FloatBits(f2.pos[0]), FloatBits(f2.pos[1]), FloatBits(f2.pos[2]));

    StateHash::Result h;
    StateHash::Compute(&h);
    char regions[400] = "";
    int len = 0;
    for (int i = 0; i < h.count && len < (int)sizeof regions - 24; ++i)
        len = AppendF(regions, sizeof regions, len, " %s=%08X", h.names[i], h.hashes[i]);
    Logger::Frame("  HASH %llu all=%08X rng=%08X draws=%d newobj=%u depth=%d%s",
                  (unsigned long long)GameLoopHook::gSimulationFrame, h.combined, h.rngState, h.rngDraws,
                  h.objectsCreated, h.scriptDepth, regions);
    CommandChannel::OnFightFrame(gameFrame);
}

// Replaces the single CALL to FightUpdatePlayers inside EngineTick.
void __cdecl HkFightUpdate() {
    InFlightGuard g;
    Rollback::OnFightUpdateEntry();
    const uint64_t before = GameLoopHook::gSimulationFrame;
    LogFightFrame();  // increments gSimulationFrame only if the fight body will run
    gFightedThisTick = GameLoopHook::gSimulationFrame != before;
    GameLoopHook::gInFightUpdate = true;
    oFightUpdate();
    GameLoopHook::gInFightUpdate = false;
    Rollback::OnFightUpdateExit();
    if (gFightedThisTick) Rollback::ReleaseDeferredActors();
    Rollback::FightTickDone(gFightedThisTick);
    if (Rollback::Enabled() && Rollback::gDiag) Snapshot::WatchBin(29, GameInfoField(MK9Game::kGameInfoGameFrame) | 0x80000000u);  // after fight
}

// Replaces engine vtable +0xBC (0x0080DD40 -> 0x007F4B00: UWorld::Tick and services).
// This is the rollback boundary: the previous frame's scheduler/physics work is joined
// and nothing of frame F's world tick has been dispatched yet.
EngineTickFn oWorldTick = nullptr;
void __fastcall HkWorldTick(void* engine, void* /*edx*/, float dt) {
    InFlightGuard g;
    ObjectHooks::gGameThread = GetCurrentThreadId();
    static bool guaranteed = false;
    if (!guaranteed) {
        // A stack overflow on this thread (run 2026-10-14 07:30, eip 0x0078907F at a prologue
        // push) left the crash handler no stack to log with; reserve some for it.
        ULONG reserve = 0x10000;
        guaranteed = true;
        if (!SetThreadStackGuarantee(&reserve)) Logger::Info("SetThreadStackGuarantee failed (%lu)", GetLastError());
    }
    CrashLog::Heartbeat(GetCurrentThreadId());
    // Canonical FP environment for the simulation (cross-machine determinism): game thread every frame, all
    // threads (task pool: anim jobs) every 600 ticks.
    FpuGuard::PinGameThread();
    static uint32_t sTicks = 0;
    static bool sWasStarting = false;
    // Not during a running netplay session: the walk (a system-wide thread snapshot, every game thread suspended for its
    // context) was one of the two long frames per 600 in LAN fights without rollbacks (net1 2026-11-04) and never
    // corrected a game thread. Once instead when a session starts waiting for its peer (paused at the first fight frame).
    const bool starting = Rollback::NetplayStarting();
    if ((sTicks++ % 600 == 0 && (FpuGuard::gPinInSessions || !Rollback::NetplayRunning())) || (starting && !sWasStarting)) {
        FramePace::Note("fpupin");
        FpuGuard::PinAllThreads();
    }
    sWasStarting = starting;
    Rollback::OnTickEntry(engine, dt);  // rollback mode: maybe roll back + resimulate, then snapshot frame F
    ObjectHooks::gPhase = 1;
    AnimInputTrace::ImagePointAt(4);  // live frame's WorldTick entry (after OnTickEntry's save)
    const int64_t tw = FramePace::Now();
    oWorldTick(engine, dt);
    FramePace::Add(FramePace::kWorldTick, FramePace::Now() - tw);
    ObjectHooks::gPhase = 0;
}

// Replaces engine vtable +0xC0. thiscall(engine, float) emulated with fastcall:
// ECX = engine, EDX unused, dt on the stack; callee pops 4 bytes in both.
void __fastcall HkEngineTick(void* engine, void* /*edx*/, float dt) {
    InFlightGuard g;
    ++gEngineTicks;
    gLastDt = dt;
    CommandChannel::RunFrame();         // scripted input / test commands, after the sampler, before the fight update
    Rollback::RecordInputs();
    gFightedThisTick = false;
    if (Rollback::Enabled() && Rollback::gDiag) Snapshot::WatchBin(29, GameInfoField(MK9Game::kGameInfoGameFrame) | 0x40000000u);  // C0 entry
    ObjectHooks::gPhase = 2;
    AnimInputTrace::ImagePointAt(5);
    const int64_t te = FramePace::Now();
    oEngineTick(engine, dt);
    FramePace::Add(FramePace::kEngineTick, FramePace::Now() - te);
    AnimInputTrace::ImagePointAt(6);  // EngineTick exit, before our between-frame work
    ObjectHooks::gPhase = 0;
    Rollback::BetweenProbeExit();
    Rollback::OnEngineTickExit();
    if (Rollback::Enabled() && Rollback::gDiag)
        for (uint32_t bin = 0; bin < 42; ++bin) Snapshot::WatchBin(bin, GameInfoField(MK9Game::kGameInfoGameFrame) | 0x20000000u);  // C0 exit, all bins
    CommandChannel::EndFrame(gFightedThisTick);

    if (gFightedThisTick) {
        if (!gCombatActive) {
            Logger::Info("Combat tick ACTIVE at engineTick=%u simFrame=%llu gf=%u (idle for %u ticks, %u gated calls total)",
                         gEngineTicks, (unsigned long long)GameLoopHook::gSimulationFrame,
                         GameInfoField(MK9Game::kGameInfoGameFrame), gIdleTicks, gSkippedCalls);
            gCombatActive = true;
            Snapshot::LogFight(GameInfoField(MK9Game::kGameInfoGameFrame));
            RngTrace::DumpIdle();
            ScriptRng::OnFightStart();
        }
        gIdleTicks = 0;
    } else {
        ++gIdleTicks;
        if (gCombatActive) {
            Logger::Info("Combat tick STOPPED at engineTick=%u simFrame=%llu gf=%u hf=%u pause=%u", gEngineTicks,
                         (unsigned long long)GameLoopHook::gSimulationFrame,
                         GameInfoField(MK9Game::kGameInfoGameFrame), GameInfoField(MK9Game::kGameInfoHostFrame),
                         Read32(MK9Game::Get().pauseFlag));
            Logger::Frame("IDLE begins at engineTick=%u", gEngineTicks);
            gCombatActive = false;
        }
    }
    // No periodic Logger::Flush any more (round 3): the logger's own thread writes both logs (Info within a few ms, Frame
    // within ~100 ms), and Flush now writes on the calling thread, which here would be the game thread inside a frame.
}
}  // namespace

namespace GameLoopHook {

uint64_t gSimulationFrame = 0;
volatile bool gInFightUpdate = false;
bool FoughtThisTick() { return gFightedThisTick; }
void ClearFoughtThisTick() { gFightedThisTick = false; }
bool FightBodyWillRun() {
    uint32_t mode, subMode, flags10, flags14;
    return FightBodyRuns(&mode, &subMode, &flags10, &flags14);
}

bool Install() {
    const MK9Game::Addresses& a = MK9Game::Get();
    oEngineTick = reinterpret_cast<EngineTickFn>(a.engineTick);
    oFightUpdate = reinterpret_cast<FightUpdateFn>(a.fightUpdatePlayers);
    oWorldTick = reinterpret_cast<EngineTickFn>(0x0080DD40);
    Rollback::SetOriginalTicks(oWorldTick, oEngineTick);

    // Inner hook first so the outer one never observes a half-installed state.
    if (!HookManager::PatchCall(a.fightUpdateCallSite, a.fightUpdatePlayers, reinterpret_cast<void*>(&HkFightUpdate),
                                "FightUpdatePlayers@callsite"))
        return false;
    if (!HookManager::PatchPointer(a.engineVtableSlot, a.engineTick, reinterpret_cast<void*>(&HkEngineTick),
                                   "EngineTick@vtable+0xC0")) {
        HookManager::RemoveAll();
        return false;
    }
    // Adjacent slot +0xBC (0x00D60DEC), verified to hold 0x0080DD40 (thiscall(engine, float), ret 4).
    if (!HookManager::PatchPointer(a.engineVtableSlot - 4, 0x0080DD40, reinterpret_cast<void*>(&HkWorldTick),
                                   "WorldTick@vtable+0xBC")) {
        HookManager::RemoveAll();
        return false;
    }
    return true;
}

void Uninstall() {
    AllocWatch::Uninstall();
    HookManager::RemoveAll();
}

long InFlight() { return gInFlight; }

}  // namespace GameLoopHook

