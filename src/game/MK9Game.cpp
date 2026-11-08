#include "MK9Game.h"

#include <windows.h>

#include <string>

#include "../common/Sha256.h"
#include "../debug/Logger.h"
#include "../hook/PatternScanner.h"

namespace {
MK9Game::Addresses gAddr{};

struct Signature {
    const char* name;
    const char* pattern;
    uintptr_t expected;  // VA in the pinned build
};

// Signatures from analysis/signatures.jsonl (REA pass 1), each verified unique
// in .text of the pinned build. Relocated absolutes and rel32s are wildcarded.
const Signature kEngineTick = {"EngineTick", "55 8B EC 83 3D ?? ?? ?? ?? 00 56 8B F1 74 05", 0x0080F670};
const Signature kFightUpdate = {"FightUpdatePlayers", "55 8B EC 83 EC 1C E8 ?? ?? ?? ?? 85 C0 0F 84 55 03 00 00",
                                0x008ACD90};
const Signature kFrameCounters = {"MainLoopFrameCounters",
                                  "A1 ?? ?? ?? ?? FF 40 6C E8 ?? ?? ?? ?? 85 C0 75 08 A1 ?? ?? ?? ?? FF 40 70",
                                  0x00404DFC};
const Signature kGetInfo = {"GetInfo", "55 8B EC 8B 45 08 83 F8 04 7C 09 B8 ?? ?? ?? ??", 0x0080E540};
const Signature kPauseGetter = {"IsPaused", "A1 ?? ?? ?? ?? C3", 0x00406790};

bool Resolve(const Signature& sig, uintptr_t textBegin, size_t textSize, uintptr_t* out) {
    auto hits = PatternScanner::FindAll(textBegin, textSize, sig.pattern);
    // IsPaused's pattern is short and not unique; require that the expected VA is among the hits.
    bool found = false;
    for (uintptr_t h : hits) found |= (h == sig.expected);
    const bool unique = hits.size() == 1;
    Logger::Info("SIG %-22s hits=%u expected=0x%08X %s", sig.name, (unsigned)hits.size(), sig.expected,
                 found ? (unique ? "OK (unique)" : "OK (expected among hits)") : "MISSING");
    if (!found) return false;
    *out = sig.expected;
    return true;
}

uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }
}  // namespace

namespace MK9Game {

const Addresses& Get() { return gAddr; }

bool VerifyBuildAndResolve(Addresses* out) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::string sha = Sha256File(exePath);
    Logger::Info("Executable: %ls", exePath);
    Logger::Info("Executable SHA256: %s", sha.empty() ? "<unreadable>" : sha.c_str());
    if (sha != kExpectedSha256) {
        Logger::Info("UNKNOWN BUILD: expected %s. Failing closed, no hooks installed.", kExpectedSha256);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    Logger::Info("Module base: 0x%08X (preferred 0x%08X)", base, kPreferredBase);
    Logger::Info("Architecture: %s", nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 ? "x86" : "UNEXPECTED");
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || base != kPreferredBase) {
        Logger::Info("Unexpected machine or relocated image. Failing closed.");
        return false;
    }

    uintptr_t textBegin = 0;
    size_t textSize = 0;
    if (!PatternScanner::TextSection(base, &textBegin, &textSize)) {
        Logger::Info(".text not found. Failing closed.");
        return false;
    }

    Addresses a{};
    uintptr_t counters = 0, getInfo = 0, isPaused = 0;
    bool ok = Resolve(kEngineTick, textBegin, textSize, &a.engineTick) &&
              Resolve(kFightUpdate, textBegin, textSize, &a.fightUpdatePlayers) &&
              Resolve(kFrameCounters, textBegin, textSize, &counters) &&
              Resolve(kGetInfo, textBegin, textSize, &getInfo) &&
              Resolve(kPauseGetter, textBegin, textSize, &isPaused);
    if (!ok) {
        Logger::Info("Signature verification failed. Failing closed.");
        return false;
    }

    // Operands pulled out of the verified instructions.
    a.gameInfoPtr = Read32(counters + 1);        // A1 <imm32>  mov eax,[gameInfoPtr]
    a.playerInfoBase = Read32(getInfo + 12);     // B8 <imm32>  mov eax,PlayerInfo[0]
    a.pauseFlag = Read32(isPaused + 1);          // A1 <imm32>  mov eax,[pauseFlag]
    a.fightUpdateCallSite = 0x0080F70B;          // verified as E8 -> fightUpdatePlayers by HookManager
    a.engineVtableSlot = 0x00D60DF0;             // verified to hold engineTick by HookManager

    Logger::Info("gameInfoPtr=0x%08X -> 0x%08X  pauseFlag=0x%08X  playerInfoBase=0x%08X", a.gameInfoPtr,
                 Read32(a.gameInfoPtr), a.pauseFlag, a.playerInfoBase);
    if (a.gameInfoPtr != 0x00DEF4B4 || a.pauseFlag != 0x00E43054 || a.playerInfoBase != 0x00E89D7C) {
        Logger::Info("Derived globals differ from the documented build. Failing closed.");
        return false;
    }

    gAddr = a;
    if (out) *out = a;
    return true;
}

}  // namespace MK9Game
