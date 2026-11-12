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

// ---- MK9's controller re-detection during a session (two-PC match of 2026-11-11) ----
// MK9's window procedure (0x005758B0, message 0x219 WM_DEVICECHANGE, any wParam) sets a flag at 0x00DE9934. The sampler
// 0x00424A50 then returns without sampling (it stores the time at 0x00E22168) and, at its first call more than 300 ms
// later, runs the re-detection 0x00422E10: release every DirectInput device, enumerate again and, for each DirectInput
// pad, ask WMI whether it is an XInput device (0x004213D0, Microsoft's IsXInputDevice: a blocking COM query over every
// Win32_PNPEntity). Windows sends that message for any change in the device tree. In a session the sampler runs at the
// boundary, so the query was one frame's time: 685 and 568 ms in that match (game thread in a COM call, a DualSense on
// the PC), and the other game waited as long.
// While local capture is on:
//  - the flag is taken before the sampler sees it (no skipped sample). 300 ms later, and once more 1.5 s after the
//    message, the game controllers Windows lists are compared with the set of the last comparison. The same set: the
//    change was not a controller's, and MK9 gets the flag back when capture ends. Another set (a pad plugged in, the
//    player's own pad lost or back): MK9 re-detects in that sample, which is what makes the pad work again;
//  - such a re-detection takes each product's XInput answer from the last real query (the call at 0x00421B14 goes
//    through MK9R_IsXInputDevice, which keeps them), so it costs DirectInput's own time; a product never asked about
//    still runs the query. Outside capture the query always runs and refreshes the kept answers.
constexpr uintptr_t kDeviceChanged = 0x00DE9934, kDeviceChangedAt = 0x00E22168;
constexpr uintptr_t kIsXInputCall = 0x00421B14, kIsXInput = 0x004213D0;
constexpr ULONGLONG kCompareAfterMs = 300, kCompareAgainMs = 1500;
// Hardware pad flags that belong to MK9's device manager (0x1 a device is attached, 0x20 its kind): set by the attach
// 0x00422FC0 and cleared by the release 0x004219D8 only, both inside the re-detection.
constexpr uint32_t kFlagsDevice = 0x21;
struct XInputAnswer {
    GUID product;
    int isXInput;
};
XInputAnswer gXInputKnown[16];
uint32_t gXInputKnownN = 0, gXInputReused = 0;
bool gXInputHooked = false;
uint32_t gPadSet = 0;           // ControllerSet at the last comparison (or at capture's start)
bool gPadSetKnown = false;
ULONGLONG gChangeAt = 0;        // a device change taken from MK9 and not compared yet (0: none)
ULONGLONG gCompareAgainAt = 0;  // the second comparison of a change whose first showed the same set (0: none)
bool gChangeOwed = false;       // a change was kept from MK9 in this capture: MK9 gets the flag back when capture ends
uint32_t gChangesKept = 0, gChangesPassed = 0;

// The game controllers Windows lists right now: HID joysticks, gamepads and multi-axis controllers of the raw input
// device list (XInput pads are in it as HID gamepads), as one order-independent number over their device handles and
// ids. A handle lasts as long as its device stays connected. No device I/O. False: the list could not be read.
bool ControllerSet(uint32_t* out) {
    RAWINPUTDEVICELIST list[128];
    UINT n = 128;
    const UINT got = GetRawInputDeviceList(list, &n, sizeof list[0]);
    if (got == (UINT)-1) return false;
    uint32_t sum = 0;
    for (UINT i = 0; i < got; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof info;
        UINT size = sizeof info;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1) continue;
        const USHORT usage = info.hid.usUsage;
        if (info.hid.usUsagePage != 1 || (usage != 4 && usage != 5 && usage != 8)) continue;
        uint32_t h = (uint32_t)reinterpret_cast<uintptr_t>(list[i].hDevice) ^ ((uint32_t)info.hid.dwVendorId << 16) ^
                     (uint32_t)info.hid.dwProductId;
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
        sum += h | 1;
    }
    *out = sum;
    return true;
}
// Before every sampler call of a capture: takes a device change MK9 has not acted on, and decides (above). True: MK9
// re-detects its controllers in the sampler call that follows.
bool HoldDeviceChange() {
    uint32_t* changed = reinterpret_cast<uint32_t*>(kDeviceChanged);
    uint32_t* changedAt = reinterpret_cast<uint32_t*>(kDeviceChangedAt);
    const ULONGLONG now = GetTickCount64();
    if (*changed) {
        *changed = 0;
        *changedAt = 0;
        if (!gChangeAt) gChangeAt = now;
    }
    const bool first = gChangeAt && now - gChangeAt >= kCompareAfterMs;
    if (!first && !(gCompareAgainAt && now >= gCompareAgainAt)) return false;
    uint32_t set = 0;
    const bool known = ControllerSet(&set);
    const bool same = known && gPadSetKnown && set == gPadSet;
    gCompareAgainAt = first && same ? gChangeAt + kCompareAgainMs : 0;
    if (first) gChangeAt = 0;
    if (same) {
        if (first) {
            gChangeOwed = true;
            if (gChangesKept++ < 8)
                Logger::Info("INPUT: Windows reported a device change and lists the same game controllers as before: MK9's "
                             "controller re-detection (a blocking query, 0.5-0.7 s of one frame) waits for the session's end");
        }
        return false;
    }
    gPadSet = set;
    gPadSetKnown = known;
    gChangeOwed = false;
    *changed = 1;
    *changedAt = (GetTickCount() - 400) | 1;  // "seen more than 300 ms ago": the sampler re-detects in this call, then samples
    ++gChangesPassed;
    Logger::Info("INPUT: the game controllers Windows lists changed during the session%s: MK9 detects its controllers again "
                 "in this frame (%u XInput answers kept from earlier queries)",
                 known ? "" : " (the list could not be read)", gXInputKnownN);
    return true;
}
// Capture starts: the controller set to compare with (none when a change is already waiting for MK9: that one may be a
// controller's, so its comparison passes it on). Capture ends: MK9 gets back a change that was kept from it.
void DeviceChangeCapture(bool on) {
    if (on) {
        gPadSetKnown = ControllerSet(&gPadSet) && !*reinterpret_cast<const uint32_t*>(kDeviceChanged);
    } else if (gChangeOwed || gChangeAt) {
        *reinterpret_cast<uint32_t*>(kDeviceChanged) = 1;  // MK9's own path from here: one skipped sample, 300 ms, re-detection
        *reinterpret_cast<uint32_t*>(kDeviceChangedAt) = 0;
        Logger::Info("INPUT: the session kept %u device change(s) from MK9 and passed %u on; MK9 detects its controllers again now "
                     "(%u XInput answers reused in the session)", gChangesKept, gChangesPassed, gXInputReused);
    }
    gChangeAt = gCompareAgainAt = 0;
    gChangeOwed = false;
    gChangesKept = gChangesPassed = gXInputReused = 0;
}
}  // namespace
}  // namespace Input

// MK9's IsXInputDevice(const GUID* guidProduct) for the DirectInput enumeration callback 0x00421AB0 (cdecl, non-zero =
// an XInput device, which the callback then leaves to XInput).
extern "C" int __cdecl MK9R_IsXInputDevice(const GUID* product) {
    using namespace Input;
    const auto query = reinterpret_cast<int(__cdecl*)(const GUID*)>(kIsXInput);
    if (!product) return query(product);
    XInputAnswer* known = nullptr;
    for (uint32_t i = 0; i < gXInputKnownN && !known; ++i)
        if (!memcmp(&gXInputKnown[i].product, product, sizeof(GUID))) known = &gXInputKnown[i];
    if (known && gCapture) {
        ++gXInputReused;
        return known->isXInput;
    }
    const int answer = query(product);
    if (!known && gXInputKnownN < 16) {
        known = &gXInputKnown[gXInputKnownN++];
        known->product = *product;
    }
    if (known) known->isXInput = answer;
    return answer;
}

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
        // The device manager's bits as they are now, not as they were when the pad was taken over: after a re-detection
        // that released this slot's device, "attached" from then sent the sampler through a null device (0x00424B1E).
        *flags = (orig & ~kFlagsDevice) | (*flags & kFlagsDevice);
        restore[k] = true;
    }
    const bool redetect = HoldDeviceChange();
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<void(__cdecl*)()>(kSampler)();
    if (redetect) {
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        Logger::Info("INPUT: MK9's controller re-detection and this sample took %.1f ms (%u XInput answers reused so far)",
                     (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart, gXInputReused);
    }
    uint32_t m = 0;
    for (int k = 0; k < 6; ++k) {
        const uintptr_t pad = kHwPads + k * kPadBytes;
        uint32_t* flagsAt = reinterpret_cast<uint32_t*>(pad + 0x08);
        const uint32_t flags = *flagsAt;
        // Only an attached pad was sampled: a pad of ours without a device still holds the session's own input.
        if ((flags & 1) && (restore[k] || !(flags & kFlagExternal))) m |= *reinterpret_cast<const uint32_t*>(pad + 0x0C);
        if (restore[k]) {
            memcpy(reinterpret_cast<void*>(pad), gPadSave[k], kPadBytes);
            *flagsAt = (*flagsAt & ~kFlagsDevice) | (flags & kFlagsDevice);  // a re-detection inside the call changed them
        }
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
    if (gSamplerHooked && !gXInputHooked) {
        const int32_t relQuery = (int32_t)(kIsXInput - (kIsXInputCall + 5));
        const int32_t relOurs = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_IsXInputDevice) - (kIsXInputCall + 5));
        memcpy(expected + 1, &relQuery, 4);
        memcpy(replacement + 1, &relOurs, 4);
        gXInputHooked = CodePatch::Write(kIsXInputCall, expected, replacement, 5, "IsXInputDevice call 0x00421B14 (controller re-detection)");
        // One re-detection now, in the menus, so the answers of the controllers in use are known before a fight.
        if (gXInputHooked && !gCapture) *reinterpret_cast<uint32_t*>(kDeviceChanged) = 1;
        else if (!gXInputHooked)
            Logger::Info("INPUT: IsXInputDevice call site not as expected; a controller change in a session runs MK9's whole query");
    }
    return gSamplerHooked;
}
void SetLocalCapture(bool on) {
    const bool was = gCapture;
    // Without the sampler patch nothing is captured: the log said "on" all the same, over a session with neutral input.
    if (on && !gSamplerHooked)
        Logger::Info("INPUT: local hardware capture stays off (the sampler call is not patched): this game's controllers "
                     "do not reach the session");
    else if (on != gCapture) Logger::Info("INPUT: local hardware capture %s", on ? "on (netplay)" : "off");
    gCapture = on && gSamplerHooked;
    gLocalHw = 0;
    if (gCapture != was) DeviceChangeCapture(gCapture);
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
