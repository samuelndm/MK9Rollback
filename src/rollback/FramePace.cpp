#include "FramePace.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/ObjectHooks.h"
#include "RenderTag.h"

namespace FramePace {
namespace {
volatile LONG64 gLiveAt = 0;  // QPC of the last live frame boundary (0: none / not in a session); 64-bit atomics (x86)
int64_t LiveAt() { return InterlockedCompareExchange64(&gLiveAt, 0, 0); }
void SetLiveAt(int64_t t) { InterlockedExchange64(&gLiveAt, t); }
volatile LONG gLiveGf = 0;
}  // namespace
namespace {
int64_t Freq() {
    static const int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return (int64_t)q.QuadPart;
    }();
    return f;
}
double Ms(int64_t t) { return t * 1000.0 / (double)Freq(); }

volatile LONG gPresent[kBuckets];  // cumulative present intervals (render thread)
volatile LONG gPresentMaxUs = 0;   // since the last report
// Controller sample -> present call of the first frame that sample changes (render thread; RenderTag carries the
// sample's time with the frame): 1 ms buckets, cumulative. LATENCY lines.
volatile LONG gLatency[kBuckets];

struct Stats {
    uint32_t hist[kBuckets];
    uint32_t frames;
    double sumMs, maxMs;
    int64_t part[kParts], partMax[kParts];
    uint32_t rollbacks, depthSum, depthMax, depthHist[10];  // depth 1..9, 10+
    uint32_t skipped;
    int64_t t0;
    uint32_t present0[kBuckets];  // gPresent at the start
    uint32_t latency0[kBuckets];  // gLatency at the start
    LONG presentMaxUs;
};
Stats gWin, gSes;
int64_t gLast = 0;
// The status line's connection-quality feed (rc25, Quality): the longest wait for the other game and the longest frame of
// this PC's own (the interval minus that wait) in the last kQualityWindowMs, with when each was measured. Anything under
// 50 ms is ordinary and not kept. Game thread only.
constexpr ULONGLONG kQualityWindowMs = 10000;
struct Peak {
    uint32_t ms = 0;
    ULONGLONG at = 0;  // GetTickCount64 when measured; 0 = none
};
Peak gNetWait, gLocalHitch;
void KeepPeak(Peak& p, double ms) {
    if (ms < 50.0) return;
    const ULONGLONG now = GetTickCount64();
    if (!p.at || now - p.at >= kQualityWindowMs || ms >= p.ms) p = {(uint32_t)ms, now};
}
int64_t gFramePart[kParts];  // the current frame's boundary parts (per-frame maxima)
uint32_t gFrameDepth = 0;    // and its deepest rollback
const char* gNotes[6];       // periodic jobs that ran in the current interval (Note)
int gNoteN = 0;
uint32_t gHitches = 0;       // intervals >= 40 ms this session
volatile LONG gReplayHitches = 0, gTakeFrames = 0;  // TakeReplayHitches (game thread writes, reads at round locks)

// Memory at long hitches (2026-11-08, field session): the friend's game stopped ~830 ms four times, twice inside the live
// save, and the mod raises the game's private memory ~650 -> ~950 MB, so paging is a suspect. Each boundary reads the
// process counters (one call, ~2 us); a HITCH >= kMemHitchMs adds the page faults of the interval (soft + hard), the
// working set and the free RAM.
constexpr double kMemHitchMs = 150.0;
using GetPmiFn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
bool ProcessCounters(DWORD* faults, SIZE_T* workingSet) {
    static const GetPmiFn fn =
        reinterpret_cast<GetPmiFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (!fn || !fn(GetCurrentProcess(), &pmc, sizeof pmc)) return false;
    *faults = pmc.PageFaultCount;
    *workingSet = pmc.WorkingSetSize;
    return true;
}
DWORD gFaultsAt = 0;  // the process's page fault count at the last boundary
bool gFaultsValid = false;

void Begin(Stats& s) {
    memset(&s, 0, sizeof s);
    s.t0 = Now();
    for (int i = 0; i < kBuckets; ++i) s.present0[i] = (uint32_t)gPresent[i];
    for (int i = 0; i < kBuckets; ++i) s.latency0[i] = (uint32_t)gLatency[i];
}
int Pct(const uint32_t* h, uint32_t n, double p) {
    if (!n) return 0;
    const uint32_t want = (uint32_t)(p * n + 0.999);
    uint32_t c = 0;
    for (int i = 0; i < kBuckets; ++i)
        if ((c += h[i]) >= want) return i;
    return kBuckets - 1;
}
uint32_t AtLeast(const uint32_t* h, int ms) {
    uint32_t c = 0;
    for (int i = ms; i < kBuckets; ++i) c += h[i];
    return c;
}
void Print(const char* tag, uint32_t gf, const Stats& s) {
    uint32_t ph[kBuckets], pn = 0;
    for (int i = 0; i < kBuckets; ++i) pn += ph[i] = (uint32_t)gPresent[i] - s.present0[i];
    const double secs = Ms(Now() - s.t0) / 1000.0;
    const uint32_t n = s.frames ? s.frames : 1;
    auto pf = [&](Part p) { return Ms(s.part[p]) / n; };
    const double other = pf(kBoundary) - pf(kLiveSave) - pf(kLoad) - pf(kReplayTick) - pf(kReplaySave) - pf(kWait);
    char dh[120];
    int k = 0;
    for (int d = 0; d < 10; ++d) k += snprintf(dh + k, sizeof dh - k, "%s%d%s:%u", d ? " " : "", d + 1, d == 9 ? "+" : "", s.depthHist[d]);
    Logger::Info("PACE %s gf=%u: %u frames in %.1f s = %.1f fps; frame ms avg %.2f p50 %d p95 %d p99 %d max %.1f, >=20ms %u >=34ms %u "
                 ">=50ms %u; presented %u (%.1f/s) ms p50 %d p95 %d p99 %d max %.1f, >=20ms %u >=34ms %u >=50ms %u; boundary ms/frame "
                 "livesave %.2f load %.2f replay %.2f replaysave %.2f wait %.2f extra %.2f other %.2f total %.2f (max %.1f, max replay "
                 "%.1f, max wait %.1f); rollbacks %u depth avg %.2f max %u [%s]; replay saves skipped %u",
                 tag, gf, s.frames, secs, secs > 0 ? s.frames / secs : 0.0, s.frames ? s.sumMs / s.frames : 0.0, Pct(s.hist, s.frames, 0.5),
                 Pct(s.hist, s.frames, 0.95), Pct(s.hist, s.frames, 0.99), s.maxMs, AtLeast(s.hist, 20), AtLeast(s.hist, 34),
                 AtLeast(s.hist, 50), pn, secs > 0 ? pn / secs : 0.0, Pct(ph, pn, 0.5), Pct(ph, pn, 0.95), Pct(ph, pn, 0.99),
                 s.presentMaxUs / 1000.0, AtLeast(ph, 20), AtLeast(ph, 34), AtLeast(ph, 50), pf(kLiveSave), pf(kLoad), pf(kReplayTick),
                 pf(kReplaySave), pf(kWait), pf(kExtra), other, pf(kBoundary), Ms(s.partMax[kBoundary]), Ms(s.partMax[kReplayTick]),
                 Ms(s.partMax[kWait]), s.rollbacks, s.rollbacks ? (double)s.depthSum / s.rollbacks : 0.0, s.depthMax, dh, s.skipped);
    // Its own line: the tools parse the PACE line's format.
    uint32_t lh[kBuckets], ln = 0;
    for (int i = 0; i < kBuckets; ++i) ln += lh[i] = (uint32_t)gLatency[i] - s.latency0[i];
    if (ln)
        Logger::Info("LATENCY %s gf=%u: controller sample to the present call of the first frame it changes (the input delay's "
                     "frames included), %u frames: ms p50 %d p95 %d p99 %d max %d, >=67ms %u >=100ms %u",
                     tag, gf, ln, Pct(lh, ln, 0.5), Pct(lh, ln, 0.95), Pct(lh, ln, 0.99), Pct(lh, ln, 1.0), AtLeast(lh, 67), AtLeast(lh, 100));
}

// Hybrid CPUs (2026-11-09, b39): the ns30 rollback hitches ran every part ~2.5x slower than average at once (load 14-20
// ms, replay saves 5-9 ms each), as on a slower core. The game thread's core class is sampled at each live boundary and
// after each load / replay save; HITCH lines say which class the interval saw and the session's end counts the frames.
using GetCpuSetsFn = BOOL(WINAPI*)(void*, ULONG, PULONG, HANDLE, ULONG);
using GetProcNumExFn = VOID(WINAPI*)(PPROCESSOR_NUMBER);
int gHybrid = -1;              // -1 not checked yet, 0 one core class (or unknown), 1 hybrid
uint8_t gCoreClass[4][64];     // [group][logical processor] -> SYSTEM_CPU_SET_INFORMATION EfficiencyClass (0xFF unknown)
uint8_t gTopClass = 0;         // the fastest class (performance cores)
GetProcNumExFn gProcNum = nullptr;
uint32_t gFrameCores = 0;      // this interval's samples: 1 a performance core, 2 an efficiency core
uint32_t gSesEFrames = 0, gSesCoreFrames = 0;
void CoreInit() {
    gHybrid = 0;
    memset(gCoreClass, 0xFF, sizeof gCoreClass);
    const HMODULE k = GetModuleHandleW(L"kernel32.dll");
    const auto sets = reinterpret_cast<GetCpuSetsFn>(reinterpret_cast<void*>(GetProcAddress(k, "GetSystemCpuSetInformation")));
    gProcNum = reinterpret_cast<GetProcNumExFn>(reinterpret_cast<void*>(GetProcAddress(k, "GetCurrentProcessorNumberEx")));
    if (!sets || !gProcNum) return;  // before Windows 10: no classes
    ULONG len = 0;
    sets(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (!len || len > (1u << 20)) return;
    std::vector<uint8_t> buf(len);
    if (!sets(buf.data(), len, &len, GetCurrentProcess(), 0)) return;
    uint8_t lo = 0xFF, hi = 0;
    // SYSTEM_CPU_SET_INFORMATION: Size +0, Type +4 (0 = CpuSetInformation), Id +8, Group +12, LogicalProcessorIndex +14,
    // EfficiencyClass +18 (higher = faster); entries are Size bytes apart.
    for (ULONG at = 0; at + 20 <= len;) {
        DWORD size = 0, type = 0;
        memcpy(&size, &buf[at], 4);
        memcpy(&type, &buf[at + 4], 4);
        if (size < 20 || at + size > len) break;
        if (type == 0) {
            WORD group = 0;
            memcpy(&group, &buf[at + 12], 2);
            const uint8_t lp = buf[at + 14], cls = buf[at + 18];
            if (group < 4 && lp < 64) gCoreClass[group][lp] = cls;
            if (cls < lo) lo = cls;
            if (cls > hi) hi = cls;
        }
        at += size;
    }
    gTopClass = hi;
    gHybrid = hi > lo ? 1 : 0;
    if (gHybrid) {
        int e = 0, n = 0;
        for (auto& g : gCoreClass)
            for (uint8_t c : g)
                if (c != 0xFF) ++n, e += c < hi;
        Logger::Info("CORES: hybrid CPU, %d of %d logical processors are efficiency cores (class < %u)", e, n, hi);
    }
}
void SampleCore() {
    if (gHybrid < 0) CoreInit();
    if (gHybrid != 1) return;
    PROCESSOR_NUMBER pn{};
    gProcNum(&pn);
    if (pn.Group >= 4 || pn.Number >= 64 || gCoreClass[pn.Group][pn.Number] == 0xFF) return;
    gFrameCores |= gCoreClass[pn.Group][pn.Number] < gTopClass ? 2u : 1u;
}
}  // namespace

void SetPowerThrottling(bool allow) {
    // PROCESS_POWER_THROTTLING_STATE {Version 1, ControlMask, StateMask}: a ControlMask bit with its StateMask bit clear
    // turns that throttling off; ControlMask 0 lets Windows decide (stock). EXECUTION_SPEED 0x1 (EcoQoS: slower clocks,
    // efficiency cores for a window it thinks is in the background), IGNORE_TIMER_RESOLUTION 0x4 (Windows 11: the
    // game's timer resolution request dropped while its window is hidden).
    using SetPiFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    const auto fn = reinterpret_cast<SetPiFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
    struct {
        ULONG Version, ControlMask, StateMask;
    } st{1, allow ? 0u : 0x5u, 0};
    BOOL ok = fn && fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st);
    if (!ok && !allow && fn) {  // Windows 10 before 11: no timer-resolution bit
        st.ControlMask = 0x1;
        ok = fn(GetCurrentProcess(), 4, &st, sizeof st);
    }
    Logger::Info("ECOQOS %d: %s (%s, error %lu)", allow ? 1 : 0,
                 allow ? "Windows decides this process's power throttling" : "power throttling off for this process (execution speed, timer resolution)",
                 ok ? (st.ControlMask == 0x1 ? "execution speed only" : "ok") : "not applied", ok ? 0ul : GetLastError());
}

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void SessionStart() {
    Begin(gSes);
    Begin(gWin);
    InterlockedExchange(&gPresentMaxUs, 0);
    gLast = 0;
    memset(gFramePart, 0, sizeof gFramePart);
    gNoteN = 0;
    gHitches = 0;
    gFrameCores = 0;
    gSesEFrames = gSesCoreFrames = 0;
}

void Note(const char* what) {
    for (int i = 0; i < gNoteN; ++i)
        if (gNotes[i] == what) return;
    if (gNoteN < (int)(sizeof gNotes / sizeof gNotes[0])) gNotes[gNoteN++] = what;
}

void LiveFrame(uint32_t gf) {
    const int64_t t = Now();
    DWORD faults = 0;
    SIZE_T workingSet = 0;
    const bool counters = ProcessCounters(&faults, &workingSet);
    SampleCore();
    if (gLast) {
        const double ms = Ms(t - gLast);
        // One line per long interval (2026-11-04: LAN fights without a rollback had 2 frames >= 50 ms per 600 frames):
        // what the previous boundary spent (the rest is that frame's tick, render and the limiter) and the periodic jobs
        // that ran in it.
        if (ms >= 40.0 && (++gHitches <= 40 || gHitches % 25 == 0 || ms >= kMemHitchMs)) {
            char notes[96] = "";
            int k = 0;
            for (int i = 0; i < gNoteN && k < (int)sizeof notes; ++i) k += snprintf(notes + k, sizeof notes - k, " %s", gNotes[i]);
            char mem[160] = "";
            if (ms >= kMemHitchMs && counters && gFaultsValid) {
                MEMORYSTATUSEX ms2{};
                ms2.dwLength = sizeof ms2;
                GlobalMemoryStatusEx(&ms2);
                snprintf(mem, sizeof mem, "; memory: %lu page faults in the interval, working set %lu MB, RAM %llu of %llu MB free (load %lu%%)",
                         (unsigned long)(faults - gFaultsAt), (unsigned long)(workingSet >> 20), (unsigned long long)(ms2.ullAvailPhys >> 20),
                         (unsigned long long)(ms2.ullTotalPhys >> 20), (unsigned long)ms2.dwMemoryLoad);
            }
            // The rest of the interval (ms - boundary - world - engine) is rendering, presenting and the limiter's wait.
            static const char* const kCores[4] = {"", "; cores P", "; cores E", "; cores P+E"};
            Logger::Info("HITCH gf=%u (#%u): %.1f ms; boundary %.1f (livesave %.1f load %.1f replay %.1f replaysave %.1f wait %.1f "
                         "extra %.1f); jobs:%s; world tick %.1f engine tick %.1f%s%s",
                         gf - 1, gHitches, ms, Ms(gFramePart[kBoundary]), Ms(gFramePart[kLiveSave]), Ms(gFramePart[kLoad]),
                         Ms(gFramePart[kReplayTick]), Ms(gFramePart[kReplaySave]), Ms(gFramePart[kWait]), Ms(gFramePart[kExtra]),
                         gNoteN ? notes : " none", Ms(gFramePart[kWorldTick]), Ms(gFramePart[kEngineTick]), mem, kCores[gFrameCores & 3]);
        }
        if (gFrameCores) {
            ++gSesCoreFrames;
            if (gFrameCores & 2) ++gSesEFrames;
        }
        InterlockedIncrement(&gTakeFrames);
        // The CPU check for a 1-frame delay: only a correction of ONE frame says something about this PC (timing audit,
        // round 3). Deeper ones come from the connection or the other game's hitches, and the session's first seconds
        // from the start itself; counted, they raised the delay for the rest of the evening on a PC that was fine.
        if (ms >= 25.0 && gFramePart[kLoad] > 0 && gFrameDepth == 1 && gSes.frames >= 300) InterlockedIncrement(&gReplayHitches);
        // Status line (rc25): the interval's wait for the other game's input, and the rest of it (this PC's own time). Not
        // in the session's first second: the start barrier waits for the other game by design.
        if (gSes.frames >= 60) {
            KeepPeak(gNetWait, Ms(gFramePart[kWait]));
            KeepPeak(gLocalHitch, ms - Ms(gFramePart[kWait]));
        }
        const int b = ms < kBuckets - 1 ? (int)ms : kBuckets - 1;
        for (Stats* s : {&gWin, &gSes}) {
            ++s->hist[b];
            ++s->frames;
            s->sumMs += ms;
            if (ms > s->maxMs) s->maxMs = ms;
            for (int p = 0; p < kParts; ++p)
                if (gFramePart[p] > s->partMax[p]) s->partMax[p] = gFramePart[p];
        }
    }
    memset(gFramePart, 0, sizeof gFramePart);
    gFrameDepth = 0;
    gNoteN = 0;
    gFrameCores = 0;
    SampleCore();  // the new interval starts where this boundary runs
    gLast = t;
    gFaultsAt = faults;
    gFaultsValid = counters;
    gLiveGf = (LONG)gf;
    SetLiveAt(t);
}

void Quality(uint16_t* waitMs, uint16_t* waitAgoS, uint16_t* hitchMs, uint16_t* hitchAgoS) {
    const ULONGLONG now = GetTickCount64();
    auto get = [now](const Peak& p, uint16_t* ms, uint16_t* ago) {
        const bool fresh = p.at && now - p.at < kQualityWindowMs;
        *ms = fresh ? (uint16_t)(p.ms < 65535 ? p.ms : 65535) : 0;
        *ago = fresh ? (uint16_t)((now - p.at) / 1000) : 0;
    };
    get(gNetWait, waitMs, waitAgoS);
    get(gLocalHitch, hitchMs, hitchAgoS);
}

void Gap() {
    SetLiveAt(0);
    gLast = 0;
    memset(gFramePart, 0, sizeof gFramePart);
    gNoteN = 0;
}

void Add(Part p, int64_t ticks) {
    if (p == kLoad || p == kReplaySave) SampleCore();
    gFramePart[p] += ticks;
    gWin.part[p] += ticks;
    gSes.part[p] += ticks;
}

uint32_t TakeReplayHitches(uint32_t* frames) {
    if (frames) *frames = (uint32_t)InterlockedExchange(&gTakeFrames, 0);
    return (uint32_t)InterlockedExchange(&gReplayHitches, 0);
}

void Rollback(uint32_t depth) {
    if (depth > gFrameDepth) gFrameDepth = depth;
    for (Stats* s : {&gWin, &gSes}) {
        ++s->rollbacks;
        s->depthSum += depth;
        if (depth > s->depthMax) s->depthMax = depth;
        ++s->depthHist[depth >= 10 ? 9 : depth ? depth - 1 : 0];
    }
}

void SkippedSave() {
    ++gWin.skipped;
    ++gSes.skipped;
}

void Report(uint32_t gf) {
    const LONG mx = InterlockedExchange(&gPresentMaxUs, 0);
    gWin.presentMaxUs = mx;
    if (mx > gSes.presentMaxUs) gSes.presentMaxUs = mx;
    Print("window", gf, gWin);
    Begin(gWin);
}

void SessionReport(const char* why) {
    SetLiveAt(0);  // the session ended: no live boundary to measure from
    const LONG mx = InterlockedExchange(&gPresentMaxUs, 0);
    if (mx > gSes.presentMaxUs) gSes.presentMaxUs = mx;
    if (gSes.frames) Print(why, 0, gSes);
    LimiterReport(why);
    if (gSesCoreFrames)
        Logger::Info("CORES %s: %u of %u live frame intervals had the game thread on an efficiency core at a sample", why, gSesEFrames,
                     gSesCoreFrames);
}

}  // namespace FramePace

// ---- limiter wait ----
// MK9's frame limiter (TimedEngineFrame 0x004025A0) waits for its deadline (double 0x00E0AB10, in seconds of
// QueryPerformanceCounter x [0x00E42FF0]) by calling Sleep(0) in a loop until 4 ms before it ([0x00C5A6B0]), then
// spinning: a whole core at 100% for the 8-12 ms a frame has to spare (heat and clock throttling on laptops, and the
// thread competes with the render thread on small CPUs). 'limiterwait 1' gives the first loop a real wait: the Sleep it
// loads at 0x004026FF (mov edi, [0x00C550AC]) becomes MK9R_LimiterSleep, which waits on a high-resolution timer until
// kLimiterEarlyMs before that 4 ms point. The game's own spin still makes the last 4 ms exact; a wake-up that comes late
// eats into it and only past it delays the frame (counted: LIMITER line at the session's end). Without the timer
// (before Windows 10 1803) it is Sleep(0) as before. Off by default until the two-PC A/B says otherwise.
namespace {
constexpr uintptr_t kLimiterSleepLoad = 0x004026FF;
constexpr uint8_t kLimiterSleepBytes[6] = {0x8B, 0x3D, 0xAC, 0x50, 0xC5, 0x00};
constexpr double kLimiterSpinMs = 4.0, kLimiterEarlyMs = 0.7;
bool gLimiterWait = false;
volatile LONG gLimiterWaits = 0, gLimiterLate = 0, gLimiterLateMaxUs = 0;
}  // namespace

extern "C" void __stdcall MK9R_LimiterSleep(DWORD ms) {
    static const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
    const double perTick = *reinterpret_cast<const double*>(0x00E42FF0);
    const double deadline = *reinterpret_cast<const volatile double*>(0x00E0AB10);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const double waitMs = (deadline - q.QuadPart * perTick) * 1000.0 - kLimiterSpinMs - kLimiterEarlyMs;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(waitMs * 10000.0);  // relative, 100 ns units
    if (!timer || waitMs < 0.5 || waitMs > 50.0 || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        Sleep(ms);
        return;
    }
    WaitForSingleObject(timer, 60);
    QueryPerformanceCounter(&q);
    const double lateMs = (q.QuadPart * perTick - deadline) * 1000.0;  // > 0: woke after the frame should have started
    InterlockedIncrement(&gLimiterWaits);
    if (lateMs > 0) {
        InterlockedIncrement(&gLimiterLate);
        const LONG us = lateMs < 2000.0 ? (LONG)(lateMs * 1000.0) : 2000000;
        if (us > gLimiterLateMaxUs) InterlockedExchange(&gLimiterLateMaxUs, us);
    }
}

namespace FramePace {
void SetLimiterWait(bool on) {
    if (on != gLimiterWait) {
        uint8_t code[6] = {0xBF, 0, 0, 0, 0, 0x90};  // mov edi, &MK9R_LimiterSleep; nop
        const uint32_t fn = (uint32_t)reinterpret_cast<uintptr_t>(&MK9R_LimiterSleep);
        memcpy(code + 1, &fn, 4);
        const bool ok = on ? CodePatch::Write(kLimiterSleepLoad, kLimiterSleepBytes, code, 6, "frame limiter Sleep 0x004026FF (limiterwait)")
                           : CodePatch::Restore(kLimiterSleepLoad);
        if (ok) gLimiterWait = on;
        else Logger::Info("LIMITERWAIT: 0x004026FF could not be %s", on ? "patched (not as expected)" : "restored");
    }
    Logger::Info("LIMITERWAIT %s", gLimiterWait ? "on: the frame limiter waits on a timer until 4.7 ms before its deadline, then spins as before"
                                                : "off: the frame limiter spins the whole wait (stock)");
}
void LimiterReport(const char* why) {
    const LONG waits = InterlockedExchange(&gLimiterWaits, 0), late = InterlockedExchange(&gLimiterLate, 0);
    const LONG maxUs = InterlockedExchange(&gLimiterLateMaxUs, 0);
    if (gLimiterWait || waits)
        Logger::Info("LIMITER %s: %ld timer waits, %ld woke after the frame's start (the latest by %.1f ms); limiterwait %d", why, waits,
                     late, maxUs / 1000.0, (int)gLimiterWait);
}
}  // namespace FramePace

namespace FramePace {

// ---- stall sampler ----
namespace {
volatile LONG gStallMs = 0;
volatile DWORD gRenderThread = 0;  // set by OnPresent (the render thread)
HANDLE gSampler = nullptr;
// A return address directly follows a call: E8 rel32, or FF /2 (call reg / [reg] / [reg+d8] / [reg+d32] / [abs32] / SIB).
bool LooksLikeReturn(uint32_t r) {
    uint32_t w0 = 0, w1 = 0;
    if (r < 0x10000 || !CrashLog::SafeRead32(r - 8, &w0) || !CrashLog::SafeRead32(r - 4, &w1)) return false;
    uint8_t b[8];
    memcpy(b, &w0, 4);
    memcpy(b + 4, &w1, 4);
    auto ff2 = [&](int at) { return b[at] == 0xFF && ((b[at + 1] >> 3) & 7) == 2; };
    return b[3] == 0xE8 || (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) ||
           (ff2(6) && (b[7] >> 6) == 0 && (b[7] & 7) != 4 && (b[7] & 7) != 5) || (ff2(5) && (b[6] >> 6) == 1 && (b[6] & 7) != 4) ||
           (ff2(4) && (b[5] >> 6) == 1 && (b[5] & 7) == 4) || (ff2(2) && (((b[3] >> 6) == 2 && (b[3] & 7) != 4) || b[3] == 0x15)) ||
           (ff2(1) && (b[2] >> 6) == 2 && (b[2] & 7) == 4);
}
bool IsCode(uint32_t a) {
    MEMORY_BASIC_INFORMATION m;
    return a >= 0x10000 && VirtualQuery(reinterpret_cast<const void*>((uintptr_t)a), &m, sizeof m) && m.State == MEM_COMMIT &&
           m.Type == MEM_IMAGE && (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
// MKKE addresses as is (they match the disassembly), others as module+offset.
int Where(uint32_t a, char* out, size_t cap) {
    if (a >= 0x00401000 && a < 0x00F9C000) return snprintf(out, cap, " %08X", a);
    HMODULE m = nullptr;
    char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>((uintptr_t)a), &m) && m && GetModuleFileNameA(m, path, sizeof path)) {
        const char* base = strrchr(path, '\\');
        return snprintf(out, cap, " %s+%X", base ? base + 1 : path, (unsigned)(a - (uint32_t)(uintptr_t)m));
    }
    return snprintf(out, cap, " %08X?", a);
}
// The game's own code or this DLL's (the rest is Windows, Steam, drivers' user-mode parts).
bool OwnCode(uint32_t a) {
    if (a >= 0x00401000 && a < 0x00F9C000) return true;
    static uint32_t base = 0, size = 0;
    if (!base) {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery(&gSampler, &m, sizeof m) && m.AllocationBase) {  // any address inside this DLL's image
            base = (uint32_t)reinterpret_cast<uintptr_t>(m.AllocationBase);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.AllocationBase);
            size = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew)->OptionalHeader.SizeOfImage;
        }
    }
    return base && a - base < size;
}
constexpr uint32_t kStackWords = 4096;
// Copies a suspended thread's context and the top of its stack (up to kStackWords dwords).
bool Capture(HANDLE t, CONTEXT* c, DWORD* stack, uint32_t* count) {
    *count = 0;
    if (SuspendThread(t) == (DWORD)-1) return false;
    c->ContextFlags = CONTEXT_CONTROL;
    const bool got = GetThreadContext(t, c) != 0;
    if (got) {
        MEMORY_BASIC_INFORMATION smbi;
        const DWORD* sp = reinterpret_cast<const DWORD*>((uintptr_t)c->Esp);
        if (VirtualQuery(sp, &smbi, sizeof smbi) && smbi.State == MEM_COMMIT) {
            const DWORD* end = reinterpret_cast<const DWORD*>(reinterpret_cast<uintptr_t>(smbi.BaseAddress) + smbi.RegionSize);
            uint32_t n = end > sp ? (uint32_t)(end - sp) : 0;
            if (n > kStackWords) n = kStackWords;
            memcpy(stack, sp, n * 4);
            *count = n;
        }
    }
    ResumeThread(t);
    return got;
}
// " eip X; stack R1 R2 ..." (after the resume: the module lookups take the loader lock, which the thread may hold).
// Return addresses in the game or this DLL and those elsewhere are counted apart: a wait deep inside Windows (the COM
// call of 2026-11-11: 18 system addresses and no caller of ours) still shows who called it. ".." = others left out.
int Describe(const CONTEXT& c, const DWORD* stack, uint32_t count, int maxOwn, int maxOther, char* out, int cap) {
    int n = snprintf(out, cap, "eip");
    n += Where((uint32_t)c.Eip, out + n, cap - n);
    n += snprintf(out + n, cap - n, "; stack");
    int own = 0, other = 0;
    bool gap = false;
    for (uint32_t i = 0; i < count && own < maxOwn && n < cap - 64; ++i) {
        const uint32_t v = stack[i];
        if (!IsCode(v) || !LooksLikeReturn(v)) continue;
        const bool ours = OwnCode(v);
        if (!ours && other >= maxOther) {
            if (!gap) n += snprintf(out + n, cap - n, " ..");
            gap = true;
            continue;
        }
        n += Where(v, out + n, cap - n);
        ++(ours ? own : other);
        gap = false;
    }
    return n;
}
HANDLE OpenFor(DWORD id, HANDLE* h, DWORD* hid) {
    if (id != *hid) {
        if (*h) CloseHandle(*h);
        *h = id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id) : nullptr;
        *hid = id;
    }
    return *h;
}
DWORD WINAPI Sampler(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // A high-resolution waitable timer (Windows 10 1803+) ticks every 2 ms without changing the system timer resolution.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    HANDLE game = nullptr, render = nullptr;
    DWORD gameId = 0, renderId = 0;
    int64_t frameKey = -1;
    int samples = 0;
    double lastMs = 0;
    uint32_t blackouts = 0;
    static DWORD stack[kStackWords], rstack[kStackWords];
    for (;;) {
        const LONG th = gStallMs;
        if (!th) {
            Sleep(100);
            continue;
        }
        // 2 ms steps for test thresholds (< 100 ms), 10 ms for the release's (120 ms): ~100 wakes a second.
        const int stepMs = th < 100 ? 2 : th < 200 ? 10 : 20;
        const int64_t w0 = Now();
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -10000LL * stepMs;  // relative, in 100 ns units
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
            else Sleep(stepMs > 1 ? stepMs / 2 : 1);
        } else {
            Sleep(stepMs > 1 ? stepMs / 2 : 1);
        }
        // This thread (highest priority, waiting on a timer) not running for 200 ms means the whole process, or the PC,
        // was stopped (the friend's ~830 ms freezes of 2026-11-07: game thread busy or blocked, or everything frozen?).
        const double slept = Ms(Now() - w0);
        if (slept >= 200.0 + stepMs && LiveAt() && blackouts++ < 30)
            Logger::Info("STALL gf=%ld: the sampler thread did not run for %.0f ms (asked %d ms): the whole game process (or the PC) was "
                         "stopped, not only the game thread", (long)gLiveGf, slept, stepMs);
        const int64_t at = LiveAt();
        const DWORD id = ObjectHooks::gGameThread;
        if (!at || !id) continue;
        const double el = Ms(Now() - at);
        if (at != frameKey) {
            frameKey = at;
            samples = 0;
            lastMs = 0;
        }
        if (el < th || el > 3000 || samples >= 5 || (samples && el - lastMs < (th < 100 ? 8 : 30))) continue;
        OpenFor(id, &game, &gameId);
        // The render thread too (Graveyard 2026-11-06: the game thread sat in the render-command fence wait 0x006360E0
        // called from the main loop at 0x00402995, so the frame's time was the render thread's).
        const DWORD rid = gRenderThread;
        OpenFor(rid != id ? rid : 0, &render, &renderId);
        if (!game || !CodePatch::TryLockSuspenders()) continue;  // CodePatch / FpuGuard / the watchdog suspend threads too
        CONTEXT c{}, rc{};
        uint32_t count = 0, rcount = 0;
        const LONG gf = gLiveGf;
        const bool got = Capture(game, &c, stack, &count);
        const bool rgot = render && Capture(render, &rc, rstack, &rcount);
        CodePatch::UnlockSuspenders();
        if (!got || LiveAt() != at) continue;  // the frame ended while we looked: not a stall sample after all
        ++samples;
        lastMs = el;
        char line[2600];
        int n = snprintf(line, sizeof line, "STALL gf=%ld +%.1f ms (sample %d): game ", (long)gf, el, samples);
        n += Describe(c, stack, count, 18, 12, line + n, (int)sizeof line - n);
        if (rgot && n < (int)sizeof line - 80) {
            n += snprintf(line + n, sizeof line - n, " | render thread %lu ", rid);
            n += Describe(rc, rstack, rcount, 14, 10, line + n, (int)sizeof line - n);
        }
        Logger::Info("%s", line);
    }
}}  // namespace

void SetStallSample(int ms) {
    ms = ms < 0 ? 0 : ms > 1000 ? 1000 : ms;
    InterlockedExchange(&gStallMs, ms);
    if (ms && !gSampler) gSampler = CreateThread(nullptr, 0, Sampler, nullptr, 0, nullptr);
    Logger::Info("STALLSAMPLE %d: %s", ms, ms ? "the game thread is sampled once a live frame runs this many ms" : "off");
}
void DefaultStallSample(int ms) {
    static bool done = false;  // once per process: an explicit 'stallsample 0' afterwards still turns it off
    if (done) return;
    done = true;
    if (!gStallMs) SetStallSample(ms);
}

void OnPresent() {
    static int64_t last = 0;  // render thread only
    gRenderThread = GetCurrentThreadId();
    const int64_t t = Now();
    if (last) {
        const int64_t us = (t - last) * 1000000 / Freq();
        const int b = us / 1000 < kBuckets - 1 ? (int)(us / 1000) : kBuckets - 1;
        InterlockedIncrement(&gPresent[b]);
        if (us > gPresentMaxUs) InterlockedExchange(&gPresentMaxUs, (LONG)(us < 0x7FFFFFFF ? us : 0x7FFFFFFF));
    }
    last = t;
    const int64_t sampled = RenderTag::TakeStamp();
    if (sampled && t >= sampled) {
        const int64_t ms = (t - sampled) * 1000 / Freq();
        InterlockedIncrement(&gLatency[ms < kBuckets - 1 ? (int)ms : kBuckets - 1]);
    }
}

}  // namespace FramePace
