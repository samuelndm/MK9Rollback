#include "PadGap.h"

#include <windows.h>
// (after windows.h)
#include <mmsystem.h>

#include <cstdio>

#include "../debug/Logger.h"

// The reading thread, the counting hook and the log lines of PadGap.h. Nothing here runs while it is off: the thread
// waits on an event and the hook is on the window's thread only while counting.
namespace PadGap {
namespace {

constexpr DWORD kReadEveryMs = 2;       // the pads' reading interval while counting
constexpr DWORD kRetryAbsentMs = 1000;  // a slot without a controller is asked again this often (the query is slow there)
constexpr double kStillMs = 34.0;       // two frames or more between two samples: the game stood still
constexpr int kLinesPerRun = 24;        // single PADGAP lines per run (the totals come at its end)

struct XState {  // XINPUT_STATE
    DWORD packet;
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT lx, ly, rx, ry;
};
using XGet = DWORD(WINAPI*)(DWORD, XState*);

volatile LONG gMode = 1, gDead = 0;

// Reading thread.
HANDLE gThread = nullptr, gQuit = nullptr, gGo = nullptr;
XGet gGet = nullptr;
Ring<512> gRing;
volatile LONG gReads = 0, gLongestReadMs = 0, gControllers = 0, gEdgesLost = 0, gResetEdges = 0;

// Game thread.
bool gRunning = false, gRunSession = false;
bool gPeriod = false;  // timeBeginPeriod(1) for a run: a 2 ms wait is a 2 ms wait, not Windows' default 15.6 ms tick
int64_t gFreq = 1;
Samples gSamples;
Press gPending[64];
int gPendingN = 0;
uint32_t gPendingLost = 0;
struct Counts {
    uint32_t samples, buttons, buttonsMissed, buttonsStill, buttonsUnknown, stick, stickMissed, stickStill;
    double longestGapMs;
} gC;
int gLines = 0;

// Keyboard: the hook runs on the window's thread, OnSample on the sampler's (both MK9's main thread).
SRWLOCK gKeysLock = SRWLOCK_INIT;
Keys gKeys;
int64_t gKeyLastSample = 0;
uint32_t gKeyPresses = 0, gKeyMissed = 0, gKeyStill = 0;
HHOOK gHook = nullptr;
DWORD gHookThread = 0;

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double Ms(int64_t ticks) { return (double)ticks * 1000.0 / (double)gFreq; }

const char* SourceName(uint32_t bit) {
    static const char* const kNames[22] = {"d-pad up", "d-pad down", "d-pad left", "d-pad right", "Start", "Back",
                                           "left stick button", "right stick button", "LB", "RB", "button 0x400", "button 0x800",
                                           "A", "B", "X", "Y", "LT", "RT", "stick up", "stick down", "stick left", "stick right"};
    for (int i = 0; i < 22; ++i)
        if (bit == (1u << i)) return kNames[i];
    return "?";
}

void KeyName(uint8_t vk, char* out, int n) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) snprintf(out, (size_t)n, "key %c", (char)vk);
    else snprintf(out, (size_t)n, "key %02X", vk);
}

LRESULT CALLBACK OnMessage(int code, WPARAM removed, LPARAM lp) {
    if (code == HC_ACTION && removed == PM_REMOVE) {
        const MSG* m = reinterpret_cast<const MSG*>(lp);
        if ((m->message == WM_KEYDOWN || m->message == WM_KEYUP) && m->wParam < (WPARAM)Keys::kKeys) {
            const uint8_t vk = (uint8_t)m->wParam;
            const int64_t t = Now();
            int verdict = -1;
            int64_t held = 0, sinceSample = 0;
            int line = 0;
            AcquireSRWLockExclusive(&gKeysLock);
            if (m->message == WM_KEYDOWN) {
                gKeys.Down(vk, t);
            } else {
                verdict = gKeys.Up(vk, t, &held);
                if (verdict >= 0) ++gKeyPresses;
                if (verdict == 1) {
                    ++gKeyMissed;
                    sinceSample = gKeyLastSample ? t - gKeyLastSample : 0;
                    if (Ms(sinceSample) >= kStillMs) ++gKeyStill;
                    line = (int)gKeyMissed;
                }
            }
            ReleaseSRWLockExclusive(&gKeysLock);
            if (verdict == 1 && line <= kLinesPerRun) {
                char name[16];
                KeyName(vk, name, sizeof name);
                Logger::Info("PADGAP: %s held %.0f ms, pressed and released with no sample of the controls in between (the last "
                             "one %.0f ms before its release%s): the game never read it",
                             name, Ms(held), Ms(sinceSample), Ms(sinceSample) >= kStillMs ? ", the game stood still" : "");
            }
        }
    }
    return CallNextHookEx(nullptr, code, removed, lp);
}

// The game's window: the one in front if it is this process's, else its first visible window without an owner.
BOOL CALLBACK FirstWindow(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(out) = w;
    return FALSE;
}
HWND GameWindow() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return w;
    w = nullptr;
    EnumWindows(FirstWindow, reinterpret_cast<LPARAM>(&w));
    return w;
}

uint32_t ReadPads(uint32_t* before, bool* present, DWORD* askAt) {
    const DWORD now = GetTickCount();
    uint32_t all = 0;
    LONG controllers = 0;
    for (DWORD i = 0; i < 4; ++i) {
        if (!present[i] && (LONG)(now - askAt[i]) < 0) continue;
        XState st{};
        if (gGet(i, &st) == ERROR_SUCCESS) {
            present[i] = true;
            before[i] = SourcesOf(st.buttons, st.leftTrigger, st.rightTrigger, st.lx, st.ly, before[i]);
            all |= before[i];
            ++controllers;
        } else {
            present[i] = false;
            before[i] = 0;
            askAt[i] = now + kRetryAbsentMs;
        }
    }
    InterlockedExchange(&gControllers, controllers);
    // A pad pressed while another program is in front is not meant for the game.
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId() ? all : 0;
}

DWORD WINAPI Reader(LPVOID) {
    const HANDLE either[2] = {gQuit, gGo};
    for (;;) {
        if (WaitForMultipleObjects(2, either, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
        Edges edges;
        uint32_t before[4] = {};
        bool present[4] = {};
        DWORD askAt[4];
        for (DWORD& t : askAt) t = GetTickCount();
        int64_t tPrev = Now();
        DWORD last = GetTickCount();
        while (WaitForSingleObject(gGo, 0) == WAIT_OBJECT_0) {
            if (InterlockedExchange(&gResetEdges, 0)) edges.Reset();
            const uint32_t all = ReadPads(before, present, askAt);
            const int64_t t = Now();
            Press out[8];
            uint32_t lost = 0;
            const int n = edges.Read(all, t, tPrev, out, 8, &lost);
            for (int i = 0; i < n; ++i) gRing.Push(out[i]);
            if (lost) InterlockedExchangeAdd(&gEdgesLost, (LONG)lost);
            tPrev = t;
            const DWORD now = GetTickCount();
            if ((LONG)(now - last) > gLongestReadMs) InterlockedExchange(&gLongestReadMs, (LONG)(now - last));
            last = now;
            InterlockedIncrement(&gReads);
            if (WaitForSingleObject(gQuit, kReadEveryMs) != WAIT_TIMEOUT) return 0;
        }
    }
}

bool StartReader() {
    if (gThread) return true;
    if (!gGet) {
        HMODULE x = GetModuleHandleW(L"xinput1_3.dll");  // what the game itself has loaded; nothing is loaded for this
        if (!x) x = GetModuleHandleW(L"xinput1_4.dll");
        if (!x) x = GetModuleHandleW(L"xinput9_1_0.dll");
        if (x) gGet = reinterpret_cast<XGet>(reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")));
        if (!gGet) return false;
    }
    if (!gQuit) gQuit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gGo) gGo = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gQuit || !gGo) return false;
    gThread = CreateThread(nullptr, 64u << 10, Reader, nullptr, 0, nullptr);
    return gThread != nullptr;
}

// A press that has ended: counted once the samples can tell. False: not yet (it ended after the last sample).
bool Decide(const Press& p) {
    const Verdict v = Judge(gSamples, p);
    if (v == Verdict::NotYet) return false;
    const bool stick = (p.source & kStick) != 0;
    if (stick) ++gC.stick;
    else ++gC.buttons;
    if (v == Verdict::Unknown) {
        if (!stick) ++gC.buttonsUnknown;
        return true;
    }
    if (v != Verdict::Missed) return true;
    int64_t before = 0, after = 0;
    gSamples.Around(p.downAfter, p.up, &before, &after);
    const double gap = before && after ? Ms(after - before) : -1.0;
    const bool still = gap >= kStillMs;
    if (stick) {
        ++gC.stickMissed;
        gC.stickStill += still;
        return true;
    }
    ++gC.buttonsMissed;
    gC.buttonsStill += still;
    if (++gLines <= kLinesPerRun)
        Logger::Info("PADGAP: %s held %.0f ms, pressed and released between two samples of the controls %.0f ms apart (%s): the "
                     "game never read it",
                     SourceName(p.source), Ms(p.up - p.down), gap,
                     gap < 0 ? "the samples around it were not kept" : still ? "the game stood still" : "a press shorter than a frame");
    return true;
}

void DecideWaiting() {
    int keep = 0;
    for (int i = 0; i < gPendingN; ++i)
        if (!Decide(gPending[i])) gPending[keep++] = gPending[i];
    gPendingN = keep;
    Press p;
    while (gRing.Pop(&p))
        if (!Decide(p)) {
            if (gPendingN < (int)(sizeof gPending / sizeof gPending[0])) gPending[gPendingN++] = p;
            else ++gPendingLost;
        }
}

void Start(bool session) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    gFreq = f.QuadPart ? f.QuadPart : 1;
    gRunning = true;
    gRunSession = session;
    gC = Counts{};
    gLines = 0;
    gSamples = Samples{};
    gPendingN = 0;
    gPendingLost = 0;
    Press p;
    while (gRing.Pop(&p)) {}  // presses of an earlier run
    gRing.dropped.store(0);
    InterlockedExchange(&gReads, 0);
    InterlockedExchange(&gLongestReadMs, 0);
    InterlockedExchange(&gEdgesLost, 0);
    InterlockedExchange(&gResetEdges, 1);
    if (!gPeriod) gPeriod = timeBeginPeriod(1) == TIMERR_NOERROR;
    const bool reader = StartReader();
    if (reader) SetEvent(gGo);
    AcquireSRWLockExclusive(&gKeysLock);
    gKeys.Reset();
    gKeyLastSample = 0;
    gKeyPresses = gKeyMissed = gKeyStill = 0;
    ReleaseSRWLockExclusive(&gKeysLock);
    const HWND w = GameWindow();
    gHookThread = w ? GetWindowThreadProcessId(w, nullptr) : 0;
    DWORD hookErr = 0;
    if (gHookThread) {
        gHook = SetWindowsHookExW(WH_GETMESSAGE, OnMessage, nullptr, gHookThread);
        if (!gHook) hookErr = GetLastError();
    }
    char kb[96];
    if (gHook) snprintf(kb, sizeof kb, "counted by a hook on the window's thread %lu", (unsigned long)gHookThread);
    else if (!gHookThread) snprintf(kb, sizeof kb, "not counted (the game's window was not found)");
    else snprintf(kb, sizeof kb, "not counted (the hook was refused, error %lu)", (unsigned long)hookErr);
    Logger::Info("PADGAP on (%s): pads %s; keyboard %s; it only counts, nothing the game reads changes",
                 session ? "a rollback session has the controls" : "'padgap 2': outside sessions too",
                 reader ? "read every 2 ms on a thread of the mod" : gGet ? "not counted (the reading thread did not start)"
                                                                      : "not counted (the game has no XInput module loaded)",
                 kb);
}

void Stop() {
    if (gGo) ResetEvent(gGo);
    if (gPeriod) {
        timeEndPeriod(1);
        gPeriod = false;
    }
    if (gHook) {
        UnhookWindowsHookEx(gHook);
        gHook = nullptr;
    }
    DecideWaiting();  // what ended before the last sample; a press still held, or ended after it, is not counted
    gRunning = false;
    AcquireSRWLockExclusive(&gKeysLock);
    const uint32_t keys = gKeyPresses, keysMissed = gKeyMissed, keysStill = gKeyStill;
    ReleaseSRWLockExclusive(&gKeysLock);
    const uint32_t lost = gPendingLost + gRing.dropped.load() + (uint32_t)gEdgesLost;
    Logger::Info("PADGAP %s: %u samples of the controls (longest gap %.0f ms); pads read %ld times (longest gap %ld ms, %ld "
                 "controller(s)): %u button press(es), %u of them never read by the game (%u while it stood still, %u shorter "
                 "than a frame), %u undecided; left stick: %u move(s), %u never read (%u while it stood still); keyboard: %u key "
                 "press(es), %u never read (%u while it stood still)%s",
                 gRunSession ? "this session" : "outside sessions", gC.samples, gC.longestGapMs, gReads, gLongestReadMs, gControllers,
                 gC.buttons, gC.buttonsMissed, gC.buttonsStill, gC.buttonsMissed - gC.buttonsStill, gC.buttonsUnknown, gC.stick,
                 gC.stickMissed, gC.stickStill, keys, keysMissed, keysStill,
                 lost ? "; some presses were not counted (too many at once)" : "");
}

}  // namespace

void SetMode(int mode) { InterlockedExchange(&gMode, mode < 0 ? 0 : mode > 2 ? 2 : mode); }
int Mode() { return (int)gMode; }

void OnSample(int64_t qpcBefore, bool session, bool panel) {
    if (gDead) return;
    const LONG mode = gMode;
    const bool want = !panel && (mode == 2 || (mode == 1 && session));
    if (want != gRunning || (want && session != gRunSession)) {
        if (gRunning) Stop();
        if (want) Start(session);
    }
    if (!gRunning) return;
    if (gSamples.n) {
        const double gap = Ms(qpcBefore - gSamples.Last());
        if (gap > gC.longestGapMs) gC.longestGapMs = gap;
    }
    gSamples.Add(qpcBefore);
    ++gC.samples;
    AcquireSRWLockExclusive(&gKeysLock);
    gKeys.Sample();
    gKeyLastSample = qpcBefore;
    ReleaseSRWLockExclusive(&gKeysLock);
    DecideWaiting();
}

void Shutdown() {
    if (InterlockedExchange(&gDead, 1)) return;
    // The hook procedure and the thread must not outlive the DLL.
    if (HHOOK h = gHook) {
        UnhookWindowsHookEx(h);
        gHook = nullptr;
    }
    if (gPeriod) {
        timeEndPeriod(1);
        gPeriod = false;
    }
    if (gThread) {
        SetEvent(gQuit);
        WaitForSingleObject(gThread, 1000);
        CloseHandle(gThread);
        gThread = nullptr;
    }
}

}  // namespace PadGap
