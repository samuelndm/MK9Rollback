#include "SoundDedupe.h"

#include <windows.h>

#include <vector>

#include "../debug/Logger.h"
#include "../hook/HookManager.h"
#include "../hook/ObjectHooks.h"
#include "RollbackManager.h"

namespace {
constexpr uintptr_t kSlot = 0x00D5E440;  // vt 0x00D5E420 + 0x20
constexpr uintptr_t kPlay = 0x00921200;
constexpr int kSlots = 32;  // frames remembered (> rollback window)

struct Rec {
    uint32_t key, arg2, handle;
    bool consumed;
};
struct FrameRecs {
    uint32_t gf = 0xFFFFFFFF;
    std::vector<Rec> recs;
};
FrameRecs gFrames[kSlots];
bool gInstalled = false;
int gDeduped = 0, gPlayed = 0;

using PlayFn = uint32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

uint32_t GameFrame() { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70); }

// arg2 == 0: arg1 is the cue name; hash its text (the pointer may differ between timelines).
uint32_t KeyOf(uint32_t a1, uint32_t a2) {
    uint32_t h = 2166136261u ^ a2;
    MEMORY_BASIC_INFORMATION mbi;
    if (a2 == 0 && a1 >= 0x10000 && VirtualQuery(reinterpret_cast<void*>(a1), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        // One query for the string's region (one per character was up to 129 syscalls per sound); the text stops at the
        // region's end as it did at the first unreadable byte.
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const char* s = reinterpret_cast<const char*>(a1);
        for (int i = 0; i < 128 && a1 + i < end && s[i]; ++i) h = (h ^ (uint8_t)s[i]) * 16777619u;
        return h;
    }
    return (h ^ a1) * 16777619u;
}

FrameRecs& Frame(uint32_t gf) {
    FrameRecs& f = gFrames[gf % kSlots];
    if (f.gf != gf) {
        f.gf = gf;
        f.recs.clear();
    }
    return f;
}
}  // namespace

// fastcall with a dummy EDX = thiscall with 7 stack arguments (callee pops 0x1C).
extern "C" uint32_t __fastcall MK9R_SoundPlay(void* self, void* /*edx*/, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                                              uint32_t a5, uint32_t a6, uint32_t a7) {
    const PlayFn play = reinterpret_cast<PlayFn>(kPlay);
    if (GetCurrentThreadId() != ObjectHooks::gGameThread || !Rollback::Enabled()) return play(self, a1, a2, a3, a4, a5, a6, a7);
    const uint32_t gf = GameFrame();
    const uint32_t key = KeyOf(a1, a2);
    FrameRecs& f = Frame(gf);
    if (Rollback::gIsResimulating) {
        for (Rec& r : f.recs)
            if (!r.consumed && r.key == key && r.arg2 == a2) {
                r.consumed = true;
                ++gDeduped;
                return r.handle;  // already playing from the original timeline
            }
        const uint32_t h = play(self, a1, a2, a3, a4, a5, a6, a7);
        f.recs.push_back({key, a2, h, true});
        ++gPlayed;
        return h;
    }
    const uint32_t h = play(self, a1, a2, a3, a4, a5, a6, a7);
    f.recs.push_back({key, a2, h, false});
    return h;
}

namespace SoundDedupe {

bool Install() {
    if (gInstalled) return true;
    gInstalled = HookManager::PatchPointer(kSlot, kPlay, reinterpret_cast<void*>(&MK9R_SoundPlay), "SoundObject vt+0x20 (play)");
    if (!gInstalled) Logger::Info("SOUNDDEDUPE: slot 0x00D5E440 not as expected; resimulated sounds play twice");
    return gInstalled;
}

void BeginResim() {
    gDeduped = gPlayed = 0;
    // Records of the window are re-marked during resimulation; earlier "consumed" flags of
    // previous rollbacks over the same frames must not hide originals.
}

Stats EndResim(uint32_t S, uint32_t F) {
    Stats s{gDeduped, gPlayed, 0};
    for (uint32_t g = S; g < F; ++g) {
        FrameRecs& f = gFrames[g % kSlots];
        if (f.gf != g) continue;
        std::vector<Rec> kept;
        for (const Rec& r : f.recs) {
            if (!r.consumed) {
                ++s.abandoned;  // played only in the abandoned timeline
                continue;
            }
            kept.push_back({r.key, r.arg2, r.handle, false});  // the corrected timeline is now the original
        }
        f.recs.swap(kept);
    }
    return s;
}

void Clear() {
    for (FrameRecs& f : gFrames) {
        f.gf = 0xFFFFFFFF;
        f.recs.clear();
    }
}

}  // namespace SoundDedupe
