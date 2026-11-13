#include "SysWatch.h"

#include <windows.h>
#include <winternl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
SysWatch::Sink gSink = nullptr;
CRITICAL_SECTION gCs;  // the sink and the line limiter: the power-mode callback arrives on a thread of the system's
bool gCsReady = false;
HWND gWnd = nullptr;
ATOM gClass = 0;
HINSTANCE gInst = nullptr;
HWINEVENTHOOK gHookSystem = nullptr, gHookShow = nullptr;
constexpr wchar_t kClass[] = L"MK9RollbackSysWatch";

// At most kPerWindow lines in kWindowMs and kPerRun in a run: a program that floods the desktop with events must not
// flood the log.
constexpr DWORD kWindowMs = 10000;
constexpr int kPerWindow = 40, kPerRun = 3000;
DWORD gWinStart = 0;
int gWinLines = 0, gWinDropped = 0, gRunLines = 0;

void Emit(const char* fmt, ...) {
    if (!gCsReady) return;
    char line[480];
    const int k = snprintf(line, sizeof line, "SYSEVENT: ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + k, sizeof line - k, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&gCs);
    if (gSink) {
        const DWORD now = GetTickCount();
        if (now - gWinStart >= kWindowMs) {
            if (gWinDropped) {
                char t[120];
                snprintf(t, sizeof t, "SYSEVENT: %d more in the %lu s before this line were not written", gWinDropped, kWindowMs / 1000);
                gSink(t);
            }
            gWinStart = now;
            gWinLines = gWinDropped = 0;
        }
        if (gWinLines < kPerWindow && gRunLines < kPerRun) {
            ++gWinLines;
            if (++gRunLines == kPerRun) gSink("SYSEVENT: the next line is number 3000 of this run and the last one written");
            gSink(line);
        } else {
            ++gWinDropped;
        }
    }
    LeaveCriticalSection(&gCs);
}

// Printable ASCII only, cut to fit: program file names, window class names, a setting's name.
void Ascii(const wchar_t* w, int len, char* out, int n) {
    int k = 0;
    for (int i = 0; i < len && w[i] && k < n - 1; ++i) out[k++] = w[i] >= 0x20 && w[i] < 0x7F ? (char)w[i] : '?';
    out[k] = 0;
}

// How much of what another program handed over can be read without a fault (bytes, at most `max`).
size_t ReadableBytes(const void* p, size_t max) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    const size_t left = (size_t)(static_cast<const char*>(mi.BaseAddress) + mi.RegionSize - static_cast<const char*>(p));
    return left < max ? left : max;
}

int Add(char* b, int n, int k, const char* fmt, ...) {
    if (k < 0 || k >= n - 1) return k;
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(b + k, (size_t)(n - k), fmt, ap);
    va_end(ap);
    return r < 0 ? k : r >= n - k ? n - 1 : k + r;
}

// ---- processor time by program ----
struct Proc {
    DWORD pid;
    LONGLONG created, kernel, user;  // 100 ns units
    char name[32];
};
constexpr int kMaxProcs = 1024;
Proc gRef[kMaxProcs], gNow[kMaxProcs];  // this module's thread only
int gRefN = 0;
LARGE_INTEGER gRefAt{};
void* gList = nullptr;
SIZE_T gListSize = 0;
using QueryFn = LONG WINAPI(ULONG infoClass, void* buffer, ULONG size, ULONG* needed);
QueryFn* gQuery = nullptr;

// Every process with its times, in the system's order. 0: not available.
int ReadProcs(Proc* out) {
    if (!gQuery) {
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        gQuery = nt ? reinterpret_cast<QueryFn*>(reinterpret_cast<void*>(GetProcAddress(nt, "NtQuerySystemInformation"))) : nullptr;
        if (!gQuery) return 0;
    }
    bool have = false;
    for (int tries = 0; tries < 4 && !have; ++tries) {
        if (!gList) {
            if (!gListSize) gListSize = 1u << 20;
            gList = VirtualAlloc(nullptr, gListSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!gList) return 0;
        }
        ULONG need = 0;
        const LONG st = gQuery(5 /* SystemProcessInformation */, gList, (ULONG)gListSize, &need);
        if (st >= 0) {
            have = true;
        } else {
            if ((ULONG)st != 0xC0000004u) return 0;  // anything but "the buffer is too small"
            VirtualFree(gList, 0, MEM_RELEASE);
            gList = nullptr;
            gListSize = ((SIZE_T)need + (256u << 10) + 0xFFFF) & ~(SIZE_T)0xFFFF;
            if (gListSize > (32u << 20)) return 0;
        }
    }
    if (!have) return 0;
    int n = 0;
    const auto* p = static_cast<const SYSTEM_PROCESS_INFORMATION*>(gList);
    const char* const end = static_cast<const char*>(gList) + gListSize;
    for (;;) {
        const DWORD pid = (DWORD)reinterpret_cast<ULONG_PTR>(p->UniqueProcessId);
        if (pid && n < kMaxProcs) {  // pid 0 is the idle time
            Proc& o = out[n++];
            o.pid = pid;
            o.created = p->CreateTime.QuadPart;
            o.kernel = p->KernelTime.QuadPart;
            o.user = p->UserTime.QuadPart;
            if (p->ImageName.Buffer && p->ImageName.Length) Ascii(p->ImageName.Buffer, p->ImageName.Length / 2, o.name, sizeof o.name);
            else snprintf(o.name, sizeof o.name, "pid %lu", pid);
        }
        if (!p->NextEntryOffset) break;
        const char* next = reinterpret_cast<const char*>(p) + p->NextEntryOffset;
        if (next < static_cast<const char*>(gList) || next + sizeof(SYSTEM_PROCESS_INFORMATION) > end) break;
        p = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(next);
    }
    return n;
}

// The program a window belongs to (its file's name only) and the window's class.
void Describe(HWND h, char* prog, int pn, char* cls, int cn) {
    wchar_t w[80];
    const int len = h ? GetClassNameW(h, w, 80) : 0;
    if (len > 0) Ascii(w, len, cls, cn);
    else snprintf(cls, (size_t)cn, "?");
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (!pid) {
        snprintf(prog, (size_t)pn, "no program");
        return;
    }
    if (pid == GetCurrentProcessId()) {
        snprintf(prog, (size_t)pn, "this program");
        return;
    }
    prog[0] = 0;
    if (const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD plen = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &plen)) {
            const wchar_t* base = path + plen;
            while (base > path && base[-1] != L'\\' && base[-1] != L'/') --base;  // the folders are not written
            Ascii(base, (int)(path + plen - base), prog, pn);
        }
        CloseHandle(p);
    }
    if (!prog[0]) {  // a protected program: its name is in the last reading of the process list, when there is one
        EnterCriticalSection(&gCs);  // the readings are another thread's
        for (int j = 0; j < gRefN; ++j)
            if (gRef[j].pid == pid) {
                snprintf(prog, (size_t)pn, "%s", gRef[j].name);
                break;
            }
        LeaveCriticalSection(&gCs);
    }
    if (!prog[0]) snprintf(prog, (size_t)pn, "a program Windows does not name (pid %lu)", pid);
}

// ---- what Windows announces ----
struct Setting {
    GUID id;
    const char* name;
};
// winnt.h's GUID_ACDC_POWER_SOURCE, GUID_BATTERY_PERCENTAGE_REMAINING, GUID_POWER_SAVING_STATUS,
// GUID_ENERGY_SAVER_STATUS, GUID_CONSOLE_DISPLAY_STATE, GUID_SESSION_DISPLAY_STATUS, GUID_MONITOR_POWER_ON,
// GUID_POWERSCHEME_PERSONALITY, GUID_SESSION_USER_PRESENCE, GUID_LIDSWITCH_STATE_CHANGE. A plan's personality is a
// GUID, written by its first eight digits: 381b4222 balanced, 8c5e7fda high performance, a1841308 power saver.
const Setting kSettings[] = {
    {{0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}}, "power source (0 mains, 1 battery, 2 short-term)"},
    {{0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}}, "battery percent"},
    {{0xe00958c0, 0xc213, 0x4ace, {0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5}}, "battery saver (1 on)"},
    {{0x550e8400, 0xe29b, 0x41d4, {0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00}}, "energy saver status"},
    {{0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}}, "display (0 off, 1 on, 2 dimmed)"},
    {{0x2b84c20e, 0xad23, 0x4ddf, {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}}, "this session's display (0 off, 1 on, 2 dimmed)"},
    {{0x02731015, 0x4510, 0x4526, {0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea}}, "monitor power (1 on)"},
    {{0x245d8541, 0x3943, 0x4422, {0xb0, 0x25, 0x13, 0xa7, 0x84, 0xf6, 0x79, 0xb7}}, "power plan personality"},
    {{0x3c0f4548, 0xc03f, 0x4c4d, {0xb9, 0xf2, 0x23, 0x7e, 0xde, 0x68, 0x63, 0x76}}, "user presence"},
    {{0xba3e0f4d, 0xb817, 0x4094, {0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3}}, "lid (1 open)"},
};
constexpr int kSettingCount = (int)(sizeof kSettings / sizeof kSettings[0]);
HPOWERNOTIFY gPower[kSettingCount] = {};

void OnPower(WPARAM code, LPARAM l) {
    if (code == PBT_APMPOWERSTATUSCHANGE) {
        SYSTEM_POWER_STATUS s{};
        GetSystemPowerStatus(&s);
        char battery[48];
        if (s.BatteryFlag & 128) snprintf(battery, sizeof battery, "no battery");
        else if (s.BatteryLifePercent > 100) snprintf(battery, sizeof battery, "battery level unknown");
        else snprintf(battery, sizeof battery, "battery %u%%, %s", s.BatteryLifePercent, (s.BatteryFlag & 8) ? "charging" : "not charging");
        Emit("the power status changed: mains %s, %s%s", s.ACLineStatus == 1 ? "connected" : s.ACLineStatus == 0 ? "NOT connected" : "unknown",
             battery, (s.SystemStatusFlag & 1) ? ", battery saver on" : "");
    } else if (code == PBT_POWERSETTINGCHANGE) {
        const auto* p = reinterpret_cast<const POWERBROADCAST_SETTING*>(l);
        const size_t head = sizeof(GUID) + sizeof(DWORD);
        if (ReadableBytes(p, head + 4) < head + 4) return;
        const char* name = "an unnamed setting";
        for (const Setting& s : kSettings)
            if (!memcmp(&s.id, &p->PowerSetting, sizeof(GUID))) name = s.name;
        DWORD v = 0;
        memcpy(&v, p->Data, 4);  // a number, or the first eight digits of a GUID
        if (p->DataLength == sizeof(GUID)) Emit("power setting: %s = %08lx", name, v);
        else Emit("power setting: %s = %lu", name, v);
    } else {
        const char* what = code == PBT_APMSUSPEND ? "the PC goes to sleep" : code == PBT_APMRESUMEAUTOMATIC || code == PBT_APMRESUMESUSPEND ? "the PC woke up" : "";
        Emit("power event %lu%s%s", (unsigned long)code, *what ? ": " : "", what);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_POWERBROADCAST:
        OnPower(w, l);
        return TRUE;
    case WM_DISPLAYCHANGE:
        Emit("the display mode changed: %ux%u, %lu bits", (unsigned)LOWORD(l), (unsigned)HIWORD(l), (unsigned long)w);
        return 0;
    case WM_DEVICECHANGE: {
        DWORD type = 0;
        const bool body = (w == 0x8000 || w == 0x8004) && ReadableBytes(reinterpret_cast<const void*>(l), 12) >= 12;  // DEV_BROADCAST_HDR
        if (body) memcpy(&type, reinterpret_cast<const char*>(l) + 4, 4);
        if (w == 0x0007) Emit("devices changed (one was added, removed or set up again)");                 // DBT_DEVNODES_CHANGED
        else if (w == 0x8000) Emit("a device arrived (type %lu; 2 is a drive)", type);                     // DBT_DEVICEARRIVAL
        else if (w == 0x8004) Emit("a device was removed (type %lu; 2 is a drive)", type);                 // DBT_DEVICEREMOVECOMPLETE
        else if (w == 0x0018) Emit("the hardware configuration changed");                                  // DBT_CONFIGCHANGED
        else Emit("device event 0x%04lX", (unsigned long)w);
        return TRUE;
    }
    case WM_SETTINGCHANGE: {
        char name[56] = "";
        const size_t bytes = ReadableBytes(reinterpret_cast<const void*>(l), 48 * sizeof(wchar_t));
        if (bytes >= sizeof(wchar_t)) Ascii(reinterpret_cast<const wchar_t*>(l), (int)(bytes / sizeof(wchar_t)), name, sizeof name);
        Emit("a system setting changed: %s (parameter %lu)", name[0] ? name : "no name given", (unsigned long)w);
        return 0;
    }
    case WM_SYSCOLORCHANGE: Emit("the system colours changed"); break;
    case WM_DEVMODECHANGE: Emit("a device's mode settings changed"); break;
    case WM_FONTCHANGE: Emit("the installed fonts changed"); break;
    case WM_TIMECHANGE: Emit("the system clock was set"); break;
    case WM_COMPACTING: Emit("Windows reports that memory is low"); break;
    case WM_THEMECHANGED: Emit("the theme changed"); break;
    case WM_DWMCOMPOSITIONCHANGED: Emit("desktop composition changed"); break;
    case WM_DWMCOLORIZATIONCOLORCHANGED: Emit("the accent colour changed"); break;
    case WM_QUERYENDSESSION: Emit("Windows asks whether the session may end"); break;
    case WM_ENDSESSION: Emit("the session ends: %s", w ? "yes" : "no"); break;
    case WM_CLOSE: return 0;  // never closed from outside
    default:
        if (m >= 0xC000) {  // a broadcast some program registered by name ("TaskbarCreated" is Explorer starting again)
            wchar_t wn[64];
            char name[64] = "?";
            const int len = GetClipboardFormatNameW(m, wn, 64);  // registered messages share the atom table of clipboard formats
            if (len > 0) Ascii(wn, len, name, sizeof name);
            Emit("a registered broadcast: %s", name);
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG idObject, LONG idChild, DWORD, DWORD ms) {
    const unsigned long ago = GetTickCount() - ms;  // raised that long before this thread got to it
    char prog[72], cls[64];
    switch (ev) {
    case EVENT_SYSTEM_FOREGROUND:
        Describe(h, prog, sizeof prog, cls, sizeof cls);
        Emit("in front now: %s, window class %s (%lu ms ago)", prog, cls, ago);
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
        Describe(h, prog, sizeof prog, cls, sizeof cls);
        Emit("a window was %s: %s, window class %s (%lu ms ago)", ev == EVENT_SYSTEM_MINIMIZESTART ? "minimized" : "restored", prog, cls, ago);
        break;
    case EVENT_SYSTEM_SWITCHSTART:
    case EVENT_SYSTEM_SWITCHEND:
        Emit("the Alt+Tab switcher %s (%lu ms ago)", ev == EVENT_SYSTEM_SWITCHSTART ? "opened" : "closed", ago);
        break;
    case EVENT_SYSTEM_DESKTOPSWITCH:
        Emit("the desktop was switched (lock screen, sign-in or an elevation prompt) (%lu ms ago)", ago);
        break;
    case EVENT_OBJECT_SHOW: {
        if (idObject != OBJID_WINDOW || idChild != 0 || !h) break;  // the window itself, not a part of one
        if (GetWindowLongW(h, GWL_STYLE) & WS_CHILD) break;
        RECT r{};
        GetWindowRect(h, &r);
        Describe(h, prog, sizeof prog, cls, sizeof cls);
        Emit("a window appeared: %s, window class %s, %ldx%ld (%lu ms ago)", prog, cls, r.right - r.left, r.bottom - r.top, ago);
        break;
    }
    default:
        break;
    }
}

// The effective power mode (powrprof.dll, Windows 10 1809 and later; looked up at run time so that an older Windows
// still loads the DLL). The callback comes on a thread of the system's, once right after registering.
using ModeFn = void WINAPI(int mode, void* context);
using ModeRegFn = HRESULT WINAPI(ULONG version, ModeFn* callback, void* context, void** handle);
using ModeUnregFn = HRESULT WINAPI(void* handle);
HMODULE gPowrprof = nullptr;
void* gModeHandle = nullptr;

void WINAPI OnMode(int mode, void*) {
    static const char* const kName[] = {"battery saver", "better battery", "balanced", "high performance", "max performance", "game mode", "mixed reality"};
    Emit("effective power mode: %s (%d)", mode >= 0 && mode < 7 ? kName[mode] : "unknown", mode);
}

// ---- the listener thread: the window, the registrations and the hooks are made, served and taken down by it ----
HANDLE gThread = nullptr, gReady = nullptr;
DWORD gThreadId = 0;
volatile LONG gUp = 0;  // the listener made at least one of its three parts

bool SetUp(SysWatch::Sink sink) {
    // The hidden window: top level (broadcasts do not reach message-only windows), never shown, never activated.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WndProc), &gInst);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = gInst;
    wc.lpszClassName = kClass;
    gClass = RegisterClassW(&wc);  // fails while a class of an earlier load of this module is still registered: then no window
    if (gClass) gWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, gInst, nullptr);
    int settings = 0;
    if (gWnd)
        for (int i = 0; i < kSettingCount; ++i) {
            gPower[i] = RegisterPowerSettingNotification(gWnd, &kSettings[i].id, DEVICE_NOTIFY_WINDOW_HANDLE);
            settings += gPower[i] != nullptr;
        }
    gHookSystem = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    gHookShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (gWnd && (gPowrprof = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) != nullptr) {
        auto* reg = reinterpret_cast<ModeRegFn*>(reinterpret_cast<void*>(GetProcAddress(gPowrprof, "PowerRegisterForEffectivePowerModeNotifications")));
        if (reg && FAILED(reg(2, OnMode, nullptr, &gModeHandle)) && FAILED(reg(1, OnMode, nullptr, &gModeHandle))) gModeHandle = nullptr;
    }
    char line[400];
    snprintf(line, sizeof line,
             "SYSWATCH: writing down what Windows announces (hidden window %s, %d of %d power settings, effective power mode %s), which "
             "program comes to the front (%s) and windows that appear (%s); program and window class names only; 'syswatch 0' turns it off",
             gWnd ? "made" : "NOT made", settings, kSettingCount, gModeHandle ? "yes" : "no", gHookSystem ? "yes" : "no", gHookShow ? "yes" : "no");
    sink(line);
    return gWnd || gHookSystem || gHookShow;
}

void TearDown() {
    if (gModeHandle && gPowrprof) {
        auto* unreg = reinterpret_cast<ModeUnregFn*>(reinterpret_cast<void*>(GetProcAddress(gPowrprof, "PowerUnregisterFromEffectivePowerModeNotifications")));
        if (unreg) unreg(gModeHandle);
    }
    gModeHandle = nullptr;
    if (gPowrprof) FreeLibrary(gPowrprof);
    gPowrprof = nullptr;
    if (gHookSystem) UnhookWinEvent(gHookSystem);
    if (gHookShow) UnhookWinEvent(gHookShow);
    gHookSystem = gHookShow = nullptr;
    for (HPOWERNOTIFY& p : gPower) {
        if (p) UnregisterPowerSettingNotification(p);
        p = nullptr;
    }
    if (gWnd) DestroyWindow(gWnd);
    gWnd = nullptr;
    MSG m;
    for (int i = 0; i < 2000 && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE); ++i) {}  // what was still queued is dropped
    if (gClass) UnregisterClassW(kClass, gInst);
    gClass = 0;
}

// It sleeps in GetMessage and handles each announcement and event the moment it arrives. It is a thread of its own
// because a thread that waits for messages waits for the window manager with them: on the watcher, whose timed key
// poll is what notices a busy window manager, that wait swallowed the poll's (one-game run of 2026-11-12: 1 KEYWAIT
// in 108 holds instead of one per hold).
DWORD WINAPI Listen(LPVOID param) {
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);  // this thread has its message queue before Start returns
    InterlockedExchange(&gUp, SetUp(reinterpret_cast<SysWatch::Sink>(param)) ? 1 : 0);
    SetEvent(gReady);
    if (gUp)
        while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);  // Stop posts WM_QUIT
    TearDown();
    return 0;
}
}  // namespace

namespace SysWatch {

bool Start(Sink sink) {
    if (gThread) return gUp != 0;
    if (!gCsReady) {
        InitializeCriticalSection(&gCs);
        gCsReady = true;
    }
    EnterCriticalSection(&gCs);
    gSink = sink;
    gWinStart = GetTickCount();
    gWinLines = gWinDropped = 0;
    LeaveCriticalSection(&gCs);
    InterlockedExchange(&gUp, 0);
    gReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (gReady) gThread = CreateThread(nullptr, 64u << 10, Listen, reinterpret_cast<LPVOID>(sink), 0, &gThreadId);
    if (gThread) WaitForSingleObject(gReady, 5000);
    if (!gThread || !gUp) {
        Stop();
        return false;
    }
    return true;
}

void Stop() {
    if (gThread) {
        PostThreadMessageW(gThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(gThread, 4000);  // it may be inside a window-manager call that is waiting
        CloseHandle(gThread);
    }
    gThread = nullptr;
    gThreadId = 0;
    if (gReady) CloseHandle(gReady);
    gReady = nullptr;
    InterlockedExchange(&gUp, 0);
    if (!gCsReady) return;
    EnterCriticalSection(&gCs);
    if (gList) VirtualFree(gList, 0, MEM_RELEASE);
    gList = nullptr;
    gRefN = 0;
    if (gSink) gSink("SYSWATCH: stopped");
    gSink = nullptr;
    LeaveCriticalSection(&gCs);
}

void Sample() {
    if (!gCsReady) return;
    EnterCriticalSection(&gCs);  // the listener reads the names (Describe)
    gRefN = ReadProcs(gRef);
    QueryPerformanceCounter(&gRefAt);
    LeaveCriticalSection(&gCs);
}

int Busiest(char* out, int n) {
    if (n < 2) return -1;
    out[0] = 0;
    if (!gRefN || !gCsReady) return -1;
    const int cur = ReadProcs(gNow);
    LARGE_INTEGER at, fq;
    QueryPerformanceCounter(&at);
    QueryPerformanceFrequency(&fq);
    if (!cur) return -1;
    struct Top {
        int i;
        LONGLONG total, kernel;
    } top[6];
    int tn = 0;
    for (int i = 0; i < cur; ++i) {
        const Proc& c = gNow[i];
        LONGLONG dk = c.kernel, du = c.user;  // a program started since the reference: all of its time
        // The system lists the programs in a steady order: the same place first, then a search.
        const Proc* r = i < gRefN && gRef[i].pid == c.pid ? &gRef[i] : nullptr;
        for (int j = 0; !r && j < gRefN; ++j)
            if (gRef[j].pid == c.pid) r = &gRef[j];
        if (r && r->created == c.created) dk -= r->kernel, du -= r->user;
        const LONGLONG total = dk + du;
        if (total < 10000) continue;  // under a millisecond
        int at2 = tn;
        while (at2 > 0 && top[at2 - 1].total < total) --at2;
        if (at2 >= 6) continue;
        if (tn < 6) ++tn;
        for (int j = tn - 1; j > at2; --j) top[j] = top[j - 1];
        top[at2] = Top{i, total, dk};
    }
    int k = 0;
    for (int t = 0; t < tn; ++t)
        k = Add(out, n, k, "%s%s %ld (kernel %ld)", t ? ", " : "", gNow[top[t].i].name, (long)(top[t].total / 10000), (long)(top[t].kernel / 10000));
    if (!tn) snprintf(out, (size_t)n, "no program used a millisecond");
    const int ms = (int)((at.QuadPart - gRefAt.QuadPart) * 1000 / (fq.QuadPart ? fq.QuadPart : 1));
    EnterCriticalSection(&gCs);
    memcpy(gRef, gNow, (size_t)cur * sizeof(Proc));
    gRefN = cur;
    gRefAt = at;
    LeaveCriticalSection(&gCs);
    return ms;
}

void* Window() { return gWnd; }

}  // namespace SysWatch
