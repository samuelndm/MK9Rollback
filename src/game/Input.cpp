#include "Input.h"

#include <windows.h>

#include <cstring>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "MK9Game.h"

namespace {
// Pad objects are long-lived; validate each distinct pointer once with VirtualQuery.
uintptr_t gValidated[4] = {};

struct Override {
    bool active;
    uintptr_t pad;
    uint32_t savedFlags;
};
Override gOverride[4] = {};

using PadEdgesFn = void(__cdecl*)(void* pad);  // 0x0087F440: cur vs prev -> press/release rings
constexpr uintptr_t kPadEdges = 0x0087F440;
constexpr uint32_t kFlagExternal = 0x8;

bool Readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) return false;
    return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uintptr_t PadOf(int player) {
    if (player < 0 || player > 3) return 0;
    const uintptr_t info = MK9Game::Get().playerInfoBase + player * MK9Game::kPlayerInfoStride;
    const uintptr_t pad = *reinterpret_cast<const uint32_t*>(info + 0xB40);
    if (pad == 0) return 0;
    if (gValidated[player] != pad) {
        if (!Readable(pad, 0x60)) return 0;
        gValidated[player] = pad;
    }
    return pad;
}
}  // namespace

namespace Input {

PadSnapshot ReadPad(int player) {
    PadSnapshot s{};
    if (player < 0 || player > 3) return s;
    const uintptr_t info = MK9Game::Get().playerInfoBase + player * MK9Game::kPlayerInfoStride;
    s.inputLock = *reinterpret_cast<const uint32_t*>(info + 0x944);
    s.pad = *reinterpret_cast<const uint32_t*>(info + 0xB40);
    const uintptr_t pad = PadOf(player);
    if (!pad) return s;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(pad);
    memcpy(&s.index, p + 0x04, 4);
    memcpy(&s.flags, p + 0x08, 4);
    memcpy(s.cur, p + 0x0C, 12);
    memcpy(s.prev, p + 0x18, 12);
    memcpy(s.ringIndex, p + 0x54, 3);
    s.valid = true;
    return s;
}

MKInput ReadPlayerInput(int player) {
    const uintptr_t pad = PadOf(player);
    return {pad ? *reinterpret_cast<const uint32_t*>(pad + 0x0C) : 0u};
}

bool OverridePlayerInput(int player, const MKInput& input) {
    const uintptr_t pad = PadOf(player);
    if (!pad) return false;
    Override& o = gOverride[player];
    if (o.active && o.pad != pad) ReleasePlayerInput(player);  // pad object changed under us
    uint32_t* flags = reinterpret_cast<uint32_t*>(pad + 0x08);
    if (!o.active) {
        o = {true, pad, *flags};
        *flags |= kFlagExternal;
        Logger::Info("INPUT override P%d on pad 0x%08X (flags 0x%08X -> 0x%08X)", player + 1, pad, o.savedFlags,
                     *flags);
    }
    uint32_t* rec = reinterpret_cast<uint32_t*>(pad + 0x0C);
    memcpy(rec + 3, rec, 12);  // prev (+0x18) = cur (+0x0C)
    rec[0] = input.mask & ~0x10000u;
    reinterpret_cast<PadEdgesFn>(kPadEdges)(reinterpret_cast<void*>(pad));
    return true;
}

namespace {
uint32_t gWanted[4] = {0, 0, 0, 0};
}
void SetWantedInput(int player, uint32_t mask) {
    if (player >= 0 && player < 4) gWanted[player] = mask;
}
uint32_t TakeWantedInput(int player) {
    if (player < 0 || player >= 4) return 0;
    const uint32_t m = gWanted[player];
    gWanted[player] = 0;
    return m;
}

void ReleasePlayerInput(int player) {
    Override& o = gOverride[player];
    if (!o.active) return;
    uint32_t* flags = reinterpret_cast<uint32_t*>(o.pad + 0x08);
    *flags = (*flags & ~kFlagExternal) | (o.savedFlags & kFlagExternal);
    // Leave a neutral record so the sampler's next prev=cur does not replay a held button.
    uint32_t* rec = reinterpret_cast<uint32_t*>(o.pad + 0x0C);
    rec[0] = 0;
    Logger::Info("INPUT release P%d (flags restored to 0x%08X)", player + 1, *flags);
    o.active = false;
}

void ReleaseAll() {
    for (int i = 0; i < 4; ++i) ReleasePlayerInput(i);
}

// Hardware pad records (6 x 0x60C at 0x00E221A8; P1 in Practice uses index 0). Menus are
// expected to read these, so this drives the front end the way a controller would.
namespace {
uint32_t gHwSavedFlags[6];
bool gHwActive[6];
}
bool OverrideHwPad(int index, uint32_t mask) {
    if (index < 0 || index > 5) return false;
    const uintptr_t pad = 0x00E221A8 + index * 0x60C;
    uint32_t* flags = reinterpret_cast<uint32_t*>(pad + 0x08);
    if (!gHwActive[index]) {
        gHwSavedFlags[index] = *flags;
        gHwActive[index] = true;
        *flags |= kFlagExternal;
        Logger::Info("INPUT hw pad %d override (flags 0x%08X)", index, gHwSavedFlags[index]);
    }
    uint32_t* rec = reinterpret_cast<uint32_t*>(pad + 0x0C);
    memcpy(rec + 3, rec, 12);
    rec[0] = mask;
    reinterpret_cast<PadEdgesFn>(kPadEdges)(reinterpret_cast<void*>(pad));
    return true;
}
void ReleaseHwPad(int index) {
    if (index < 0 || index > 5 || !gHwActive[index]) return;
    const uintptr_t pad = 0x00E221A8 + index * 0x60C;
    *reinterpret_cast<uint32_t*>(pad + 0x08) =
        (*reinterpret_cast<uint32_t*>(pad + 0x08) & ~kFlagExternal) | (gHwSavedFlags[index] & kFlagExternal);
    *reinterpret_cast<uint32_t*>(pad + 0x0C) = 0;
    gHwActive[index] = false;
    Logger::Info("INPUT hw pad %d released", index);
}
bool HwPadOverridden(int index) { return index >= 0 && index < 6 && gHwActive[index]; }

// ---- local player's real input during netplay ----
// A fighter pad we drive is externally driven (flags |= 8), so the hardware sampler 0x00424A50 skips it, and the
// player's own controller/keyboard never reached the session. The sampler's only call (0x00404E26, main loop) goes
// through MK9R_Sampler: hardware pads we took over are handed back to the sampler for this one call with their bytes
// saved, their sampled masks are read, and the saved bytes are put back (prev/cur and the press/release rings are
// gameplay state and must only change through the synchronized input).
namespace {
constexpr uintptr_t kSamplerCall = 0x00404E26, kSampler = 0x00424A50;
constexpr uintptr_t kHwPads = 0x00E221A8;
constexpr uint32_t kPadBytes = 0x60C;
bool gCapture = false, gSamplerHooked = false;
uint32_t gLocalHw = 0;
int64_t gLocalHwQpc = 0;  // when gLocalHw was sampled (QueryPerformanceCounter)
uint8_t gPadSave[6][kPadBytes];
bool OriginalFlags(uintptr_t pad, int k, uint32_t* out) {
    for (const Override& o : gOverride)
        if (o.active && o.pad == pad) {
            *out = o.savedFlags;
            return true;
        }
    if (gHwActive[k]) {
        *out = gHwSavedFlags[k];
        return true;
    }
    return false;
}
}  // namespace
}  // namespace Input

namespace Input {
namespace {
// Late sampling (rc21, 2026-11-09 latency audit): the main loop samples the controllers (0x00404E26) before
// TimedEngineFrame's frame-limiter wait, so the mask the session's boundary (WorldTick entry, after that wait) sends is as
// old as the wait: 3-11 ms. While a P2P session runs ('latesample 1', SetDeferSampling), the main loop's call is
// deferred to the boundary's start (SampleDeferredNow), one sampler call per main-loop pass as before; a pass whose
// boundary did not run (a paused tick) samples at the next pass's usual call instead.
volatile bool gDefer = false;
bool gPending = false;
uint32_t gDeferredSamples = 0, gUndeferredSamples = 0;
void SampleCapture();
}  // namespace
}  // namespace Input

extern "C" void __cdecl MK9R_Sampler() {
    using namespace Input;
    if (!gCapture) {
        gPending = false;
        reinterpret_cast<void(__cdecl*)()>(kSampler)();
        return;
    }
    if (gDefer) {
        if (!gPending) {  // sampled at this pass's boundary
            gPending = true;
            return;
        }
        ++gUndeferredSamples;  // the last pass's boundary did not run: sample now, at the usual time
        gPending = false;
    } else {
        gPending = false;
    }
    SampleCapture();
}

namespace Input {
namespace {
void SampleCapture() {
    bool restore[6] = {};
    for (int k = 0; k < 6; ++k) {
        const uintptr_t pad = kHwPads + k * kPadBytes;
        uint32_t* flags = reinterpret_cast<uint32_t*>(pad + 0x08);
        uint32_t orig;
        if (!(*flags & kFlagExternal) || !OriginalFlags(pad, k, &orig) || (orig & kFlagExternal)) continue;
        memcpy(gPadSave[k], reinterpret_cast<const void*>(pad), kPadBytes);
        *flags = orig;
        restore[k] = true;
    }
    reinterpret_cast<void(__cdecl*)()>(kSampler)();
    uint32_t m = 0;
    for (int k = 0; k < 6; ++k) {
        const uintptr_t pad = kHwPads + k * kPadBytes;
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(pad + 0x08);
        if (restore[k] || ((flags & 1) && !(flags & kFlagExternal))) m |= *reinterpret_cast<const uint32_t*>(pad + 0x0C);
        if (restore[k]) memcpy(reinterpret_cast<void*>(pad), gPadSave[k], kPadBytes);
    }
    gLocalHw = m & 0xFFFFu;
    LARGE_INTEGER sampledAt;
    QueryPerformanceCounter(&sampledAt);
    gLocalHwQpc = sampledAt.QuadPart;
    static uint32_t sLoggedKey = ~0u, sNonZero = 0;
    uint32_t key = 0;
    for (int k = 0; k < 6; ++k) {
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(kHwPads + k * kPadBytes + 0x08);
        if (restore[k]) key |= 1u << (k + 8);
        else if ((flags & 1) && !(flags & kFlagExternal)) key |= 1u << k;
    }
    if (key != sLoggedKey) {  // which pads feed the local input (bits 0-5 sampled normally, 8-13 ours, restored)
        Logger::Info("INPUT capture: hardware pads sampled %02X, ours sampled and restored %02X", key & 0x3F, key >> 8);
        sLoggedKey = key;
    }
    if (gLocalHw && sNonZero++ < 5) Logger::Info("INPUT capture: local hardware mask %04X", gLocalHw);
}
}  // namespace

void SetDeferSampling(bool on) {
    if (on == gDefer) return;
    gDefer = on;
    if (!on && gPending) {  // a deferred sample not yet taken: take it now
        gPending = false;
        if (gCapture) SampleCapture();
    }
}
void SampleDeferredNow() {
    if (!gPending) return;
    gPending = false;
    ++gDeferredSamples;
    if (gCapture) SampleCapture();
}
void LogSamplingCounts() {
    if (gDeferredSamples || gUndeferredSamples)
        Logger::Info("LATESAMPLE: %u controller samples taken at the boundary, %u at the main loop's usual call (the pass "
                     "before had no boundary)", gDeferredSamples, gUndeferredSamples);
    gDeferredSamples = gUndeferredSamples = 0;
}

bool InstallSamplerHook() {
    if (gSamplerHooked) return true;
    uint8_t expected[5] = {0xE8}, replacement[5] = {0xE8};
    const int32_t relOld = (int32_t)(kSampler - (kSamplerCall + 5));
    const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_Sampler) - (kSamplerCall + 5));
    memcpy(expected + 1, &relOld, 4);
    memcpy(replacement + 1, &relNew, 4);
    gSamplerHooked = CodePatch::Write(kSamplerCall, expected, replacement, 5, "hardware sampler call (local netplay input)");
    if (!gSamplerHooked) Logger::Info("INPUT: sampler call site not as expected; local controllers cannot drive netplay");
    return gSamplerHooked;
}
void SetLocalCapture(bool on) {
    if (on != gCapture) Logger::Info("INPUT: local hardware capture %s", on ? "on (netplay)" : "off");
    gCapture = on && gSamplerHooked;
    gLocalHw = 0;
}
uint32_t LocalHardwareMask() { return gCapture ? gLocalHw : 0; }
int64_t LocalSampleQpc() { return gCapture ? gLocalHwQpc : 0; }

bool IsOverridden(int player) { return player >= 0 && player < 4 && gOverride[player].active; }

// The fighters' input history at a netplay start: cur/prev records (+0x0C/+0x18), +0x24..+0x53, the three 30-entry
// rings of 16-byte {button, GameFrame stamp, ...} records (write indices +0x54..+0x56, rings +0x58/+0x238/+0x418,
// written by 0x00425770/0x004256A0 from the edge pass 0x0087F440) and the tail +0x5F8..; plus each player's
// last-6-buttons list at 0x00E22170 + player*0x1C (index +0x18). Between rounds nothing is synchronized, so each game
// entered round 2 with its own history: P1 prev 0x24 on one, 0x1000 on the other, rings 13/19/13 vs 14/21/14, and the
// first frame's input became a different press (two-instance regression 2026-10-25: fight 2 split at frame 1, 9 vs
// 3 RNG draws; round 1, entered from a neutral menu, matched). Cleared to the same neutral state on both peers;
// the header (+0x00 owner, +0x04 index, +0x08 flags) is kept.
int CanonicalizePadsForNetplay() {
    int n = 0;
    for (int p = 0; p < 2; ++p) {
        const uintptr_t pad = PadOf(p);
        if (!pad || !Readable(pad, 0x60C)) continue;
        memset(reinterpret_cast<void*>(pad + 0x0C), 0, 0x60C - 0x0C);
        const uintptr_t owner = *reinterpret_cast<const uint32_t*>(pad);
        if (owner && Readable(owner + 0xB3C, 4)) {
            const uint32_t idx = *reinterpret_cast<const uint32_t*>(owner + 0xB3C);
            if (idx < 2) memset(reinterpret_cast<void*>(0x00E22170 + idx * 0x1C), 0, 0x1C);
        }
        ++n;
    }
    return n;
}

}  // namespace Input
