// Why the overlay reads the F9 key on a thread of its own (src\debug\Overlay.cpp, "the watcher thread"), shown on the
// PC this runs on.
//
// GetAsyncKeyState enters the kernel's window manager and waits for its lock, which every program of the desktop
// session shares. A program that keeps that lock busy makes each such call in every other program wait as long. Up to
// rc31 the overlay made that call once per frame on MKKE's render thread, so a busy window manager became a hitch in
// the fight (docs\todo.md, "Stalls of 150-240 ms near a match's end"). PeekMessage on an empty queue and
// GetForegroundWindow answer from memory the process reads itself and do not wait; they are measured next to it.
//
// Three threads call one function each, once a millisecond, and keep their longest call. The main thread keeps the
// window manager busy: SetWindowRgn on a window whose region has that many one-pixel rectangles, replacing it by one
// with half the rows, then by the full one again, and so on. Measured on the PC this was written on (1920x1200): a
// window's first region and a region over the same rows cost under 2 ms; one over other rows about 40 ms at 100,000
// rectangles and 80-95 ms from 800,000 to 2,400,000 (no longer with more: the rows below the screen's edge add
// little); 2,800,000 are refused. The window is fully transparent, click-through, never activated and has no
// taskbar button.
//
//   winlock_probe.exe [rectangles=800000] [holds=3] [gap ms=1500] [lead ms=0]
//
// It is also the lock holder of a two-game test: start it while two games fight; each hold's line starts with the
// wall-clock time, to be matched with the HITCH / KEYWAIT / OVERLAYSLOW lines of the games' logs.
//
// NOT one of the default host tests: during a hold every program on the PC waits, a running game too. run.ps1
// -Only winlock refuses to start while MKKE runs.
#include <windows.h>

#include <mmsystem.h>  // timeBeginPeriod (WIN32_LEAN_AND_MEAN leaves it out of windows.h)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
LARGE_INTEGER gF;
double MsSince(const LARGE_INTEGER& a) {
    LARGE_INTEGER b;
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)gF.QuadPart;
}

enum Kind { kAsync, kForeground, kPeek, kKinds };
std::atomic<bool> gRun{true};
std::atomic<long long> gMaxUs[kKinds];

DWORD WINAPI Poller(LPVOID arg) {
    const Kind kind = (Kind)(intptr_t)arg;
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // this thread has a message queue, like a game's main thread
    while (gRun) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        if (kind == kAsync) GetAsyncKeyState(VK_F9);
        else if (kind == kForeground) GetForegroundWindow();
        else PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
        QueryPerformanceCounter(&b);
        const long long us = (b.QuadPart - a.QuadPart) * 1000000 / gF.QuadPart;
        long long m = gMaxUs[kind].load();
        while (us > m && !gMaxUs[kind].compare_exchange_weak(m, us)) {}
        Sleep(1);
    }
    return 0;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

constexpr int kPerRow = 800;  // one-pixel rectangles on every other pixel of a 1600 pixel row

// n one-pixel rectangles, on the even (shift 0) or the odd (shift 1) pixels of each row.
HRGN MakeRegion(int n, int shift) {
    std::vector<char> buf(sizeof(RGNDATAHEADER) + (size_t)n * sizeof(RECT));
    auto* rd = reinterpret_cast<RGNDATA*>(buf.data());
    rd->rdh.dwSize = sizeof(RGNDATAHEADER);
    rd->rdh.iType = RDH_RECTANGLES;
    rd->rdh.nCount = (DWORD)n;
    rd->rdh.nRgnSize = (DWORD)((size_t)n * sizeof(RECT));
    SetRect(&rd->rdh.rcBound, 0, 0, kPerRow * 2, (n + kPerRow - 1) / kPerRow);
    auto* r = reinterpret_cast<RECT*>(rd->Buffer);
    for (int i = 0; i < n; ++i) {
        const int x = (i % kPerRow) * 2 + shift, y = i / kPerRow;
        SetRect(&r[i], x, y, x + 1, y + 1);
    }
    return ExtCreateRegion(nullptr, (DWORD)buf.size(), rd);
}

int gChecks = 0, gFailed = 0;
void Check(bool ok, const char* what, double got, double limit) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s (%.2f ms against %.2f ms)\n", what, got, limit);
}

// The window's own thread must keep answering between the holds.
void WaitPumping(int ms) {
    const DWORD end = GetTickCount() + (DWORD)ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        const int left = (int)(end - GetTickCount());
        if (left <= 0) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)left, QS_ALLINPUT);
    }
}
}  // namespace

int main(int argc, char** argv) {
    const int rects = argc > 1 ? atoi(argv[1]) : 800000;
    const int holds = argc > 2 ? atoi(argv[2]) : 3;
    const int gapMs = argc > 3 ? atoi(argv[3]) : 1500;
    const int leadMs = argc > 4 ? atoi(argv[4]) : 0;
    if (rects < 1000 || rects > 2400000 || holds < 1 || holds > 500 || gapMs < 100 || gapMs > 600000 || leadMs < 0 || leadMs > 600000) {
        printf("winlock_probe.exe [rectangles 1000..2400000] [holds 1..500] [gap ms 100..600000] [lead ms 0..600000]\n");
        return 2;
    }
    QueryPerformanceFrequency(&gF);
    timeBeginPeriod(1);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MK9RWinlockProbe";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_POPUP, 0, 0,
                             kPerRow * 2, (rects + kPerRow - 1) / kPerRow, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) {
        printf("FAIL: no window (error %lu)\n", GetLastError());
        return 1;
    }
    SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);  // fully transparent
    ShowWindow(w, SW_SHOWNOACTIVATE);
    HRGN first = MakeRegion(rects, 1);
    if (!first || !SetWindowRgn(w, first, TRUE)) {  // the system owns a region it accepted
        printf("FAIL: the first region of %d rectangles was refused (error %lu)\n", rects, GetLastError());
        return 1;
    }

    HANDLE th[kKinds];
    for (int k = 0; k < kKinds; ++k) th[k] = CreateThread(nullptr, 0, Poller, (LPVOID)(intptr_t)k, 0, nullptr);
    WaitPumping(leadMs > 300 ? leadMs : 300);

    int compared = 0;
    for (int i = 0; i < holds; ++i) {
        // Half the rows, then all of them again, and so on. Building the region is not window-manager work.
        HRGN rgn = MakeRegion((i & 1) ? rects : rects / 2, i & 1);
        for (auto& m : gMaxUs) m = 0;
        SYSTEMTIME st;
        GetLocalTime(&st);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        const int ok = rgn ? SetWindowRgn(w, rgn, TRUE) : 0;
        const double held = MsSince(t);
        Sleep(30);  // the pollers' calls that waited have returned
        const double async = (double)gMaxUs[kAsync].load() / 1000.0, fg = (double)gMaxUs[kForeground].load() / 1000.0,
                     peek = (double)gMaxUs[kPeek].load() / 1000.0;
        printf("hold %d at %02u:%02u:%02u.%03u: the window manager was busy %.1f ms%s | GetAsyncKeyState waited %.1f ms | "
               "GetForegroundWindow %.2f ms | PeekMessage (empty queue) %.2f ms\n",
               i + 1, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, held, ok ? "" : " (the region was REFUSED)", async, fg, peek);
        fflush(stdout);
        if (held >= 20.0) {  // shorter than that, scheduling noise is as large as the thing measured
            ++compared;
            Check(async >= held / 2, "GetAsyncKeyState did not wait for the window manager", async, held / 2);
            Check(fg < held / 4, "GetForegroundWindow waited for the window manager", fg, held / 4);
            Check(peek < held / 4, "PeekMessage on an empty queue waited for the window manager", peek, held / 4);
        }
        if (i + 1 < holds) WaitPumping(gapMs);
    }
    gRun = false;
    WaitForMultipleObjects(kKinds, th, TRUE, 2000);
    DestroyWindow(w);
    timeEndPeriod(1);
    if (!compared) printf("no hold reached 20 ms on this PC, so nothing was compared (more rectangles make a longer hold)\n");
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (gFailed) return 1;
    printf("ALL CHECKS PASSED\n");
    return 0;
}
