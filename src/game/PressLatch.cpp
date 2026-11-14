#include "PressLatch.h"

#include <windows.h>

#include <cstdio>

#include "../debug/Logger.h"

// The hook, the reading thread and the log lines of PressLatch.h. Nothing here runs unless a panel of the mod has the
// controls: the hook is on the window's thread from Begin to End, and the thread waits on an event in between.
namespace PressLatch {
namespace {

constexpr DWORD kReadEveryMs = 4;       // the pads' reading interval while a panel is open
constexpr DWORD kRetryAbsentMs = 1000;  // a slot without a controller is asked again this often (the query is slow there)

struct XState {  // XINPUT_STATE
    DWORD packet;
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT lx, ly, rx, ry;
};
using XGet = DWORD(WINAPI*)(DWORD, XState*);

// Off at start since the stage screen opens by itself in online matches (2026-11-13): the message hook and the pad
// thread have run in no game yet, and their first run is not to be a players' match. 'stagelatch 1' turns it on.
volatile LONG gWanted = 0, gDead = 0;
bool gOpen = false;  // between Begin and End (game thread)

// Keyboard. The hook runs on the window's thread and OnSample on the sampler's; in MK9 both are the main thread, but
// nothing here relies on it.
SRWLOCK gKeysLock = SRWLOCK_INIT;
KeyStretch gKeys;
HHOOK gHook = nullptr;
DWORD gHookThread = 0;
volatile LONG gKeyLines = 0;
uint32_t gHeldAtBegin = 0, gExpiredAtBegin = 0;

// Pads.
Latch gLatch;
Trust gTrust;  // sampler's thread
HANDLE gThread = nullptr, gQuit = nullptr, gGo = nullptr;
XGet gGet = nullptr;
volatile LONG gReads = 0, gLongestMs = 0, gControllers = 0;
uint32_t gPadAdded = 0;

uint32_t NowMs() { return GetTickCount(); }

void Post(const KeyStretch::Up* ups, int n) {
    for (int i = 0; i < n; ++i) {
        if (PostMessageW(reinterpret_cast<HWND>(ups[i].window), WM_KEYUP, ups[i].vk, (LPARAM)ups[i].lParam)) continue;
        const DWORD err = GetLastError();
        AcquireSRWLockExclusive(&gKeysLock);
        gKeys.PostFailed(ups[i].vk);
        ReleaseSRWLockExclusive(&gKeysLock);
        Logger::Info("INPUT latch: the held release of key %02X could not be posted to the game's window (error %lu); the game "
                     "has the key down until it is pressed again", ups[i].vk, (unsigned long)err);
    }
}

LRESULT CALLBACK OnMessage(int code, WPARAM removed, LPARAM lp) {
    if (code == HC_ACTION && removed == PM_REMOVE) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        const bool key = (m->message == WM_KEYDOWN || m->message == WM_KEYUP) && m->wParam < (WPARAM)KeyStretch::kKeys;
        KeyStretch::Up ups[8];
        int n = 0;
        bool held = false;
        const uint32_t now = NowMs();
        AcquireSRWLockExclusive(&gKeysLock);
        if (key && m->message == WM_KEYDOWN) gKeys.KeyDown((uint8_t)m->wParam, now);
        else if (key) held = gKeys.KeyUp((uint8_t)m->wParam, reinterpret_cast<uintptr_t>(m->hwnd), (uintptr_t)m->lParam, now);
        n = gKeys.Expire(now, ups, 8);
        ReleaseSRWLockExclusive(&gKeysLock);
        if (held) {
            if (InterlockedIncrement(&gKeyLines) <= 16)
                Logger::Info("INPUT latch: key %02X was pressed and released between two samples of the controls; its release "
                             "waits until MK9 has sampled the key", (unsigned)m->wParam);
            m->message = WM_NULL;  // the game takes nothing from this message
        }
        if (n) {
            Post(ups, n);
            Logger::Info("INPUT latch: %d key release(s) given back after %u ms without a sample of the controls", n,
                         KeyStretch::kMaxHoldMs);
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

void ReadPads(uint32_t* before, bool* present, DWORD* askAt) {
    const DWORD now = GetTickCount();
    uint32_t all = 0;
    LONG controllers = 0;
    for (DWORD i = 0; i < 4; ++i) {
        if (!present[i] && (LONG)(now - askAt[i]) < 0) continue;
        XState st{};
        if (gGet(i, &st) == ERROR_SUCCESS) {
            present[i] = true;
            before[i] = SourcesOf(st.buttons, st.lx, st.ly, before[i]);
            all |= before[i];
            ++controllers;
        } else {
            present[i] = false;
            before[i] = 0;
            askAt[i] = now + kRetryAbsentMs;
        }
    }
    InterlockedExchange(&gControllers, controllers);
    // A pad pressed while another program is in front is not meant for the panel.
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    gLatch.Read(pid == GetCurrentProcessId() ? all : 0);
}

DWORD WINAPI Reader(LPVOID) {
    const HANDLE either[2] = {gQuit, gGo};
    for (;;) {
        if (WaitForMultipleObjects(2, either, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
        uint32_t before[4] = {};
        bool present[4] = {};
        DWORD askAt[4];
        for (DWORD& t : askAt) t = GetTickCount();
        DWORD last = GetTickCount();
        while (WaitForSingleObject(gGo, 0) == WAIT_OBJECT_0) {
            ReadPads(before, present, askAt);
            const DWORD now = GetTickCount();
            if ((LONG)(now - last) > gLongestMs) InterlockedExchange(&gLongestMs, (LONG)(now - last));
            last = now;
            InterlockedIncrement(&gReads);
            if (WaitForSingleObject(gQuit, kReadEveryMs) != WAIT_TIMEOUT) return 0;
        }
        gLatch.Clear();
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

const char* SourceNames(uint32_t sources, char* out, int n) {
    static const char* const kNames[kSources] = {"d-pad up", "d-pad down", "d-pad left", "d-pad right", "A",
                                                 "stick up", "stick down", "stick left", "stick right"};
    int at = 0;
    out[0] = 0;
    for (int i = 0; i < kSources; ++i)
        if (sources & (1u << i)) at += snprintf(out + at, at < n ? (size_t)(n - at) : 0, "%s%s", at ? ", " : "", kNames[i]);
    return out;
}

}  // namespace

void SetWanted(bool on) { InterlockedExchange(&gWanted, on ? 1 : 0); }
bool Wanted() { return gWanted != 0; }

void Begin() {
    if (gOpen || !gWanted || gDead) return;
    gOpen = true;
    AcquireSRWLockExclusive(&gKeysLock);
    gKeys.Reset();
    gHeldAtBegin = gKeys.heldBack;
    gExpiredAtBegin = gKeys.expired;
    ReleaseSRWLockExclusive(&gKeysLock);
    InterlockedExchange(&gKeyLines, 0);
    InterlockedExchange(&gReads, 0);
    InterlockedExchange(&gLongestMs, 0);
    gPadAdded = 0;
    const HWND w = GameWindow();
    gHookThread = w ? GetWindowThreadProcessId(w, nullptr) : 0;
    DWORD hookErr = 0;
    if (gHookThread) {
        gHook = SetWindowsHookExW(WH_GETMESSAGE, OnMessage, nullptr, gHookThread);
        if (!gHook) hookErr = GetLastError();
    }
    gLatch.Clear();
    const bool reader = StartReader();
    if (reader) SetEvent(gGo);
    char kb[96];
    if (gHook) snprintf(kb, sizeof kb, "releases wait for a sample (hook on the window's thread %lu%s)", (unsigned long)gHookThread,
                        gHookThread == GetCurrentThreadId() ? ", the game thread" : "");
    else if (!gHookThread) snprintf(kb, sizeof kb, "as before (the game's window was not found)");
    else snprintf(kb, sizeof kb, "as before (the hook was refused, error %lu)", (unsigned long)hookErr);
    Logger::Info("INPUT latch: on for this panel; keyboard: %s; pads: %s", kb,
                 reader ? "read every 4 ms on a thread of the mod, a button counts once MK9's sampler has shown it too"
                        : gGet ? "as before (the reading thread did not start)" : "as before (the game has no XInput module loaded)");
}

void End() {
    if (!gOpen) return;
    gOpen = false;
    if (gGo) ResetEvent(gGo);
    uint32_t held = 0, expired = 0;
    if (gHook) {
        KeyStretch::Up ups[16];
        int n;
        do {
            AcquireSRWLockExclusive(&gKeysLock);
            n = gKeys.TakeAll(NowMs(), ups, 16);
            if (n < 16) {
                held = gKeys.heldBack - gHeldAtBegin;
                expired = gKeys.expired - gExpiredAtBegin;
                gKeys.Reset();
            }
            ReleaseSRWLockExclusive(&gKeysLock);
            Post(ups, n);
        } while (n == 16);
        UnhookWindowsHookEx(gHook);
        gHook = nullptr;
    }
    Logger::Info("INPUT latch: off with the panel; %u key release(s) were held for a sample (%u given back unsampled), %u pad "
                 "press(es) added between samples; pads read %ld times (longest gap %ld ms), %ld controller(s) at the end",
                 held, expired, gPadAdded, gReads, gLongestMs, gControllers);
}

void OnSample(uint32_t sampledMask) {
    if (!gOpen) return;
    if (gHook) {
        KeyStretch::Up ups[16];
        AcquireSRWLockExclusive(&gKeysLock);
        const int n = gKeys.Sample(NowMs(), ups, 16);
        ReleaseSRWLockExclusive(&gKeysLock);
        Post(ups, n);
    }
    if (gThread) {
        uint32_t lost = 0;
        const uint32_t gained = gTrust.Observe(gLatch.Held(), sampledMask, &lost);
        char names[160];
        if (gained)
            Logger::Info("INPUT latch: MK9's sampler shows the same as the pad reading for: %s (sampled mask %04X); from now on a "
                         "press of it between two samples reaches the panel", SourceNames(gained, names, sizeof names), sampledMask);
        if (lost)
            Logger::Info("INPUT latch: held on the pad but not in MK9's samples for %d samples: %s (sampled mask %04X); not used "
                         "for the panel until the two agree again", Trust::kDiffer, SourceNames(lost, names, sizeof names), sampledMask);
    }
}

uint32_t TakePad() {
    if (!gOpen || !gThread) return 0;
    return gTrust.Mask(gLatch.Take());
}

void NotePadAdded(uint32_t bits) {
    if (gPadAdded++ < 16) Logger::Info("INPUT latch: pad press %04X fell between two samples of the controls; the panel gets it", bits);
}

void Shutdown() {
    if (InterlockedExchange(&gDead, 1)) return;
    // The game thread may still be inside Begin / End / OnSample for a moment (its hooks come out after this); what must
    // not outlive the DLL is the hook procedure and the thread, and a release held back must reach the game.
    if (HHOOK h = gHook) {
        KeyStretch::Up ups[16];
        int n;
        do {
            AcquireSRWLockExclusive(&gKeysLock);
            n = gKeys.TakeAll(NowMs(), ups, 16);
            ReleaseSRWLockExclusive(&gKeysLock);
            for (int i = 0; i < n; ++i) PostMessageW(reinterpret_cast<HWND>(ups[i].window), WM_KEYUP, ups[i].vk, (LPARAM)ups[i].lParam);
        } while (n == 16);
        UnhookWindowsHookEx(h);
        gHook = nullptr;
    }
    if (gThread) {
        SetEvent(gQuit);
        WaitForSingleObject(gThread, 1000);
        CloseHandle(gThread);
        gThread = nullptr;
    }
}

void Describe(char* out, int n) {
    if (n <= 0) return;
    char names[160];
    snprintf(out, (size_t)n, "%s%s; keyboard hook %s; %u key release(s) held for a sample so far; pads: %ld controller(s) at the last "
             "reading, known to agree with MK9's sampler: %s",
             gWanted ? "on" : "off ('stagelatch 1' turns it on)", gOpen ? ", a panel is open" : "", gHook ? "in place" : "not in place",
             gKeys.heldBack, gControllers, gTrust.known ? SourceNames(gTrust.known, names, sizeof names) : "nothing yet");
}

}  // namespace PressLatch
