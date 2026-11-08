#include "FramePace.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/ObjectHooks.h"

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

struct Stats {
    uint32_t hist[kBuckets];
    uint32_t frames;
    double sumMs, maxMs;
    int64_t part[kParts], partMax[kParts];
    uint32_t rollbacks, depthSum, depthMax, depthHist[10];  // depth 1..9, 10+
    uint32_t skipped;
    int64_t t0;
    uint32_t present0[kBuckets];  // gPresent at the start
    LONG presentMaxUs;
};
Stats gWin, gSes;
int64_t gLast = 0;
int64_t gFramePart[kParts];  // the current frame's boundary parts (per-frame maxima)
const char* gNotes[6];       // periodic jobs that ran in the current interval (Note)
int gNoteN = 0;
uint32_t gHitches = 0;       // intervals >= 40 ms this session

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
}
}  // namespace

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
            Logger::Info("HITCH gf=%u (#%u): %.1f ms; boundary %.1f (livesave %.1f load %.1f replay %.1f replaysave %.1f wait %.1f "
                         "extra %.1f); jobs:%s; world tick %.1f engine tick %.1f%s",
                         gf - 1, gHitches, ms, Ms(gFramePart[kBoundary]), Ms(gFramePart[kLiveSave]), Ms(gFramePart[kLoad]),
                         Ms(gFramePart[kReplayTick]), Ms(gFramePart[kReplaySave]), Ms(gFramePart[kWait]), Ms(gFramePart[kExtra]),
                         gNoteN ? notes : " none", Ms(gFramePart[kWorldTick]), Ms(gFramePart[kEngineTick]), mem);
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
    gNoteN = 0;
    gLast = t;
    gFaultsAt = faults;
    gFaultsValid = counters;
    gLiveGf = (LONG)gf;
    SetLiveAt(t);
}

void Gap() {
    SetLiveAt(0);
    gLast = 0;
    memset(gFramePart, 0, sizeof gFramePart);
    gNoteN = 0;
}

void Add(Part p, int64_t ticks) {
    gFramePart[p] += ticks;
    gWin.part[p] += ticks;
    gSes.part[p] += ticks;
}

void Rollback(uint32_t depth) {
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
}

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
// Copies a suspended thread's context and the top of its stack (up to 1024 dwords).
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
            if (n > 1024) n = 1024;
            memcpy(stack, sp, n * 4);
            *count = n;
        }
    }
    ResumeThread(t);
    return got;
}
// " eip X; stack R1 R2 ..." (after the resume: the module lookups take the loader lock, which the thread may hold).
int Describe(const CONTEXT& c, const DWORD* stack, uint32_t count, int maxShown, char* out, int cap) {
    int n = snprintf(out, cap, "eip");
    n += Where((uint32_t)c.Eip, out + n, cap - n);
    n += snprintf(out + n, cap - n, "; stack");
    int shown = 0;
    for (uint32_t i = 0; i < count && shown < maxShown && n < cap - 64; ++i) {
        const uint32_t v = stack[i];
        if (!IsCode(v) || !LooksLikeReturn(v)) continue;
        n += Where(v, out + n, cap - n);
        ++shown;
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
    static DWORD stack[1024], rstack[1024];
    for (;;) {
        const LONG th = gStallMs;
        if (!th) {
            Sleep(100);
            continue;
        }
        // 2 ms steps for test thresholds (< 100 ms), 20 ms for the release's (250 ms): ~50 wakes a second.
        const int stepMs = th < 100 ? 2 : 20;
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
        if (el < th || el > 3000 || samples >= 5 || (samples && el - lastMs < 8)) continue;
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
        n += Describe(c, stack, count, 18, line + n, (int)sizeof line - n);
        if (rgot && n < (int)sizeof line - 80) {
            n += snprintf(line + n, sizeof line - n, " | render thread %lu ", rid);
            n += Describe(rc, rstack, rcount, 24, line + n, (int)sizeof line - n);
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
}

}  // namespace FramePace
