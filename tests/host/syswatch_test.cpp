// src\debug\SysWatch: the hidden window turns what Windows announces into SYSEVENT lines (the announcements are posted
// to that one window here, nothing is broadcast), a window of another program that appears is named by its program
// and class, the processor-time list names a program that worked, a flood of events is cut, and Stop leaves nothing
// behind. Not covered: a real change of the foreground (the test would have to take the user's foreground away).
//   syswatch_test.exe            the test (about 12 s: it waits for the line limiter's ten seconds to pass)
//   syswatch_test.exe window     the "other program": a fully transparent 48x32 window for half a second
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "SysWatch.h"

namespace {
int gChecks = 0, gFailed = 0;
CRITICAL_SECTION gCs;
std::vector<std::string> gLines;

void Sink(const char* line) {
    EnterCriticalSection(&gCs);
    gLines.emplace_back(line);
    LeaveCriticalSection(&gCs);
}
void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s\n", what);
}
int Count(const char* part) {
    EnterCriticalSection(&gCs);
    int n = 0;
    for (const std::string& l : gLines) n += l.find(part) != std::string::npos;
    LeaveCriticalSection(&gCs);
    return n;
}
bool Has(const char* part) { return Count(part) > 0; }
void PumpFor(int ms) { Sleep((DWORD)ms); }  // the listener thread handles what arrives; this thread only waits

LRESULT CALLBACK HelperProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
int HelperWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = HelperProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MK9RSysWatchTestWindow";
    RegisterClassW(&wc);
    const HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_POPUP, 0, 0, 48,
                                   32, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return 1;
    SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
    ShowWindow(w, SW_SHOWNOACTIVATE);
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 500) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
    DestroyWindow(w);
    return 0;
}

DWORD WINAPI Spin(LPVOID) {
    const DWORD start = GetTickCount();
    volatile unsigned x = 0;
    while (GetTickCount() - start < 300) x = x * 1664525u + 1013904223u;
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "window")) return HelperWindow();
    InitializeCriticalSection(&gCs);
    const DWORD t0 = GetTickCount();

    Check(SysWatch::Start(Sink), "Start");
    const HWND w = static_cast<HWND>(SysWatch::Window());
    Check(w != nullptr && IsWindow(w), "the hidden window exists");
    Check(w && !IsWindowVisible(w), "the hidden window is not visible");
    Check(w && GetAncestor(w, GA_PARENT) == GetDesktopWindow(), "the hidden window is a top-level window (broadcasts reach it)");
    Check(w && GetWindowThreadProcessId(w, nullptr) != GetCurrentThreadId(), "the hidden window belongs to the listener thread, not to the caller");
    Check(Has("SYSWATCH: writing down"), "the start line");
    PumpFor(400);
    const int initial = Count("power setting: ");
    printf("note: %d power setting(s) reported their value at the start; effective power mode reported: %s\n", initial,
           Has("effective power mode: ") ? "yes" : "no");
    Check(initial >= 1, "at least one power setting reports its value when the registration is made");

    // Announcements, posted to the hidden window only.
    static const wchar_t kArea[] = L"ImmersiveColorSet";
    struct {
        GUID id;
        DWORD len;
        UCHAR data[16];
    } percent{{0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}}, 4, {79}},
      plan{{0x245d8541, 0x3943, 0x4422, {0xb0, 0x25, 0x13, 0xa7, 0x84, 0xf6, 0x79, 0xb7}}, 16, {0x78, 0x56, 0x34, 0x12}},
      unknown{{0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}}, 4, {5}};
    struct {
        DWORD size, type, reserved;
    } drive{12, 2, 0};
    const UINT registered = RegisterWindowMessageW(L"MK9RSysWatchTestBroadcast");
    // Windows refuses to post a message whose parameter is a pointer; those are sent (they return when the listener
    // has handled them).
    PostMessageW(w, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0);
    SendMessageW(w, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&percent));
    SendMessageW(w, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&plan));
    SendMessageW(w, WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&unknown));
    PostMessageW(w, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1200));
    PostMessageW(w, WM_DEVICECHANGE, 7, 0);
    SendMessageW(w, WM_DEVICECHANGE, 0x8000, reinterpret_cast<LPARAM>(&drive));
    SendMessageW(w, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kArea));
    SendMessageW(w, WM_SETTINGCHANGE, 47, 0);
    PostMessageW(w, WM_TIMECHANGE, 0, 0);
    PostMessageW(w, registered, 0, 0);
    PostMessageW(w, WM_CLOSE, 0, 0);
    // (A parameter that is not a readable address cannot be sent from here: Windows reads it on the sender's side, in
    // this thread, before the window sees anything. The module's own guard against one is not covered by this test.)
    PumpFor(200);
    Check(Has("SYSEVENT: the power status changed: mains "), "power status change");
    Check(Has("SYSEVENT: power setting: battery percent = 79"), "a power setting with a number");
    Check(Has("SYSEVENT: power setting: power plan personality = 12345678"), "a power setting with a GUID");
    Check(Has("SYSEVENT: power setting: an unnamed setting = 5"), "a power setting this module has no name for");
    Check(Has("SYSEVENT: the display mode changed: 1920x1200, 32 bits"), "display mode change");
    Check(Has("SYSEVENT: devices changed"), "device change");
    Check(Has("SYSEVENT: a device arrived (type 2"), "a device arrival");
    Check(Has("SYSEVENT: a system setting changed: ImmersiveColorSet (parameter 0)"), "setting change with a name");
    Check(Has("SYSEVENT: a system setting changed: no name given (parameter 47)"), "setting change without a name");
    Check(Has("SYSEVENT: the system clock was set"), "clock change");
    Check(Count("SYSEVENT: a registered broadcast: MK9RSysWatchTestBroadcast") == 1, "a registered broadcast's name");
    Check(IsWindow(w), "WM_CLOSE from outside does not close the hidden window");

    // A window of another program appears.
    wchar_t self[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    swprintf(cmd, MAX_PATH + 16, L"\"%ls\" window", self);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const bool started = CreateProcessW(self, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0;
    Check(started, "the helper program starts");
    PumpFor(900);
    if (started) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    Check(Has("SYSEVENT: a window appeared: syswatch_test.exe, window class MK9RSysWatchTestWindow, 48x32"),
          "another program's new window is named by program, class and size");

    // Processor time by program.
    char who[420];
    Check(SysWatch::Busiest(who, sizeof who) == -1, "no list without a reference reading");
    LARGE_INTEGER a, b, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 20; ++i) SysWatch::Sample();
    QueryPerformanceCounter(&b);
    printf("note: one reading of every program's processor time takes %.2f ms here\n", (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart / 20);
    const HANDLE spin = CreateThread(nullptr, 0, Spin, nullptr, 0, nullptr);
    WaitForSingleObject(spin, 5000);
    CloseHandle(spin);
    const int ms = SysWatch::Busiest(who, sizeof who);
    printf("list over %d ms: %s\n", ms, who);
    Check(ms >= 250 && ms < 5000, "the list's time span is the time since the reference reading");
    const char* me = strstr(who, "syswatch_test.exe ");
    Check(me != nullptr, "the list names this program, which worked for 300 ms");
    Check(me && atoi(me + 18) >= 200 && atoi(me + 18) <= 450, "with about 300 ms of processor time");

    // A flood is cut: at most 40 lines in ten seconds, and the next line after those says how many were left out.
    const char* const kFlood = "SYSEVENT: a registered broadcast: MK9RSysWatchTestBroadcast";
    for (int i = 0; i < 100; ++i) PostMessageW(w, registered, 0, 0);
    PumpFor(300);
    const int flood = Count(kFlood);
    printf("note: 100 events at once gave %d lines\n", flood - 1);
    Check(flood >= 2 && flood <= 41, "100 events at once give at most 40 lines");
    while (GetTickCount() - t0 < 10600) PumpFor(100);
    PostMessageW(w, registered, 0, 0);
    PumpFor(100);
    Check(Has(" more in the 10 s before this line were not written"), "the count of the lines left out");
    Check(Count(kFlood) == flood + 1, "after the ten seconds lines are written again");

    SysWatch::Stop();
    WNDCLASSW wc{};
    Check(SysWatch::Window() == nullptr && !IsWindow(w), "Stop destroys the hidden window");
    Check(!GetClassInfoW(GetModuleHandleW(nullptr), L"MK9RollbackSysWatch", &wc), "Stop unregisters the window class");
    Check(Has("SYSWATCH: stopped"), "the stop line");
    Check(SysWatch::Busiest(who, sizeof who) == -1, "no list after Stop");
    const int lines = (int)gLines.size();
    Check(SysWatch::Start(Sink) && SysWatch::Window() != nullptr, "a second Start (the DLL injected again)");
    PumpFor(100);
    SysWatch::Stop();
    Check(SysWatch::Window() == nullptr && (int)gLines.size() > lines, "and its Stop");

    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (gFailed || (argc > 1 && !strcmp(argv[1], "lines")))  // what was written, for a look (build\hosttests\syswatch_test.txt)
        for (const std::string& l : gLines) printf("line: %s\n", l.c_str());
    if (gFailed) return 1;
    printf("ALL CHECKS PASSED\n");
    return 0;
}
