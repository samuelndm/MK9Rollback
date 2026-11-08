#include "ParticleDone.h"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "RollbackManager.h"

namespace {
constexpr uintptr_t kSite = 0x004366A6;  // mov eax, [eax+0x2C0] (component flags), then shr eax,2 / and eax,1
constexpr uintptr_t kBack = 0x004366AC;
const uint8_t kOrig[6] = {0x8B, 0x80, 0xC0, 0x02, 0x00, 0x00};
constexpr uint32_t kCompleted = 0x4;

// MK id of the particle -> first live frame its component was seen completed (game thread only). Keyed by id, not by
// component: a replay re-creates an object born inside the window with the same id but a new actor and component.
std::unordered_map<uint32_t, uint32_t> gSeen;
bool gInstalled = false;
uint32_t gHeld = 0, gCalls = 0;

uint32_t GameFrame() { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70); }
}  // namespace

extern "C" uint32_t __fastcall MK9R_PartDoneFlags(uint32_t obj, uint32_t comp) {
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(comp + 0x2C0);
    const uint32_t gf = GameFrame();
    if (++gCalls % 4096 == 0)  // stamps older than any loadable frame are not needed any more
        for (auto it = gSeen.begin(); it != gSeen.end();)
            it = (gf - it->second > 600 && it->second - gf > 600) ? gSeen.erase(it) : std::next(it);
    if (!(flags & kCompleted)) return flags;
    const uint32_t id = *reinterpret_cast<const uint32_t*>(obj + 4);
    auto it = gSeen.find(id);
    if (it == gSeen.end()) {
        // Replays only re-run frames the live timeline already ran, and every live frame runs this check for every
        // particle. A completion first seen in a replay was never seen live: it is the abandoned future's (the live
        // particle died another way first, e.g. with its parent; soak 10006 S=1727, killer 0x004366BF in the replay
        // only). Not recorded: the next live frame records it when it is real.
        if (Rollback::gIsResimulating) {
            if (gHeld++ < 10)
                Logger::Info("PARTDONE: particle %08X (id %08X): component completed only in the abandoned future; at gf %u (replay) "
                             "it still runs",
                             obj, id, gf);
            return flags & ~kCompleted;
        }
        gSeen[id] = gf;
        return flags;
    }
    if (gf < it->second && it->second - gf < 600) {
        if (gHeld++ < 10)
            Logger::Info("PARTDONE: particle %08X (id %08X): its component completed at gf %u in the original timeline; at gf %u "
                         "(replay) it still runs",
                         obj, id, it->second, gf);
        return flags & ~kCompleted;
    }
    return flags;
}

extern "C" void MK9R_PartDoneStub();
asm(R"(
    .text
    .globl _MK9R_PartDoneStub
_MK9R_PartDoneStub:
    pushl %ecx
    pushl %edx
    movl %eax, %edx
    movl %esi, %ecx
    call @MK9R_PartDoneFlags@8
    popl %edx
    popl %ecx
    pushl $0x004366AC
    ret
)");

namespace ParticleDone {

bool Install() {
    if (gInstalled) return true;
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_PartDoneStub) - (kSite + 5));
    memcpy(jmp + 1, &rel, 4);
    gInstalled = CodePatch::Write(kSite, kOrig, jmp, 6, "MK particle completion read 0x004366A6 (replay-consistent)");
    if (!gInstalled) Logger::Info("PARTDONE: 0x004366A6 not as expected; particles may die early in replays");
    (void)kBack;
    return gInstalled;
}

void Clear() { gSeen.clear(); }

uint32_t Held() { return gHeld; }

}  // namespace ParticleDone
