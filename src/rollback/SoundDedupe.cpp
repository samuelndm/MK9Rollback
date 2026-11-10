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
constexpr int kNearMax = 4;
constexpr int kLateFrames = 3;  // a new sound this many frames behind its own frame is counted as audibly late

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
int gNear = 2;  // 'soundnear'

// The rollback bracket in progress: the replayed window [gWinS, gWinF) and its counts.
bool gBracket = false;
uint32_t gWinS = 0, gWinF = 0;
SoundDedupe::Stats gNow{};

struct Totals {
    uint32_t rollbacks, audible, deduped, shifted, played, late, abandoned;
    int maxLate;
};
Totals gSession{};

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

// The original sound a replayed one continues: an unconsumed record of the same cue on the replayed frame itself, else
// on the nearest frame of the replayed window (earlier first: 0, -1, +1, -2, +2). A record found on another frame moves
// to this one, where the corrected timeline plays it, so a later rollback over these frames finds it on its own frame.
bool TakeOriginal(uint32_t gf, uint32_t key, uint32_t arg2, uint32_t* handle) {
    for (int i = 0; i <= 2 * gNear; ++i) {
        const int d = (i & 1) ? -(i + 1) / 2 : i / 2;
        const uint32_t g = gf + (uint32_t)d;
        if (d && (!gBracket || g < gWinS || g >= gWinF)) continue;  // frames outside the window keep their own sounds
        FrameRecs& from = gFrames[g % kSlots];
        if (from.gf != g) continue;
        for (size_t k = 0; k < from.recs.size(); ++k) {
            Rec& r = from.recs[k];
            if (r.consumed || r.key != key || r.arg2 != arg2) continue;
            *handle = r.handle;
            if (d == 0) {
                r.consumed = true;
            } else {
                const Rec moved{key, arg2, r.handle, true};
                from.recs.erase(from.recs.begin() + k);
                Frame(gf).recs.push_back(moved);
                ++gNow.shifted;
            }
            ++gNow.deduped;
            return true;
        }
    }
    return false;
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
        uint32_t original = 0;
        if (TakeOriginal(gf, key, a2, &original)) return original;  // already playing from the original timeline
        const uint32_t h = play(self, a1, a2, a3, a4, a5, a6, a7);
        f.recs.push_back({key, a2, h, true});
        ++gNow.played;
        const uint32_t late = gBracket && gWinF > gf ? gWinF - gf : 0;  // frames since the frame it belongs to was shown
        if (late < (uint32_t)kSlots && (int)late > gNow.maxLate) gNow.maxLate = (int)late;
        if (late >= (uint32_t)kLateFrames && late < (uint32_t)kSlots) ++gSession.late;
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

void BeginResim(uint32_t S, uint32_t F) {
    if (!gBracket || F != gWinF) {  // F differs: the previous bracket never closed (its session stopped inside it)
        gBracket = true;
        gNow = {};
        gWinS = S;
        gWinF = F;
    } else if (S < gWinS) {
        gWinS = S;
    }
    // "Consumed" means "heard in the replay that is about to run". A second load of the same boundary replays these
    // frames again: what the first replay matched or started must be matchable, not played once more. (Between
    // brackets EndResim leaves every kept record unconsumed, so the first load finds nothing to reset.)
    for (uint32_t g = S; g < F && g - S < (uint32_t)kSlots; ++g) {
        FrameRecs& f = gFrames[g % kSlots];
        if (f.gf != g) continue;
        for (Rec& r : f.recs) r.consumed = false;
    }
}

Stats EndResim(uint32_t S, uint32_t F) {
    Stats s = gNow;
    for (uint32_t g = S; g < F && g - S < (uint32_t)kSlots; ++g) {
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
    gBracket = false;
    gNow = {};
    ++gSession.rollbacks;
    if (s.played || s.abandoned) ++gSession.audible;
    gSession.deduped += s.deduped;
    gSession.shifted += s.shifted;
    gSession.played += s.played;
    gSession.abandoned += s.abandoned;
    if (s.maxLate > gSession.maxLate) gSession.maxLate = s.maxLate;
    return s;
}

void Clear() {
    for (FrameRecs& f : gFrames) {
        f.gf = 0xFFFFFFFF;
        f.recs.clear();
    }
    gBracket = false;
    gNow = {};
}

void SetNearFrames(int frames) {
    gNear = frames < 0 ? 0 : frames > kNearMax ? kNearMax : frames;
    Logger::Info("SOUNDDEDUPE: replayed sounds match originals up to %d frame(s) away%s", gNear, gNear ? "" : " (the exact frame only)");
}

void SessionReset() { gSession = {}; }

void LogSession() {
    Logger::Info("SOUND this session: %u of %u rollbacks changed what is heard; replayed sounds: %u already playing (%u of them "
                 "a frame or two off: kept, not doubled), %u started late (%u by %d+ frames, the latest %d), %u from a "
                 "wrong prediction played on (not stopped); soundnear %d",
                 gSession.audible, gSession.rollbacks, gSession.deduped, gSession.shifted, gSession.played, gSession.late,
                 kLateFrames, gSession.maxLate, gSession.abandoned, gNear);
}

}  // namespace SoundDedupe
