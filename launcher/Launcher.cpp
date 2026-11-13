// "MK9 Rollback.exe" (x86): the program a player double-clicks.
// It opens the MK9 Rollback window (tools\netplay_gui.ps1, run by Windows PowerShell) without a console window, shows
// a small "Starting" card until that window is up, and says in a message box why when the window cannot open.
// It replaces "Play Online.bat" (until 2026-11: a .bat that started PowerShell from a console).
//
//   MK9 Rollback.exe [arguments]     the arguments go to tools\netplay_gui.ps1 as they are (-NoUpdateCheck, ...)
//
// The folder: the scripts are in tools\ next to this program (a release), or two folders up (build\<name>\ in the
// source tree). Nothing is installed and nothing is written outside %TEMP%.
// One window at a time: the window owns the mutex kWindowMutex; when another one already has it, this program asks it
// to come to the front (kShowEvent) and ends. Two quick double-clicks start one window (kStartingMutex).
// Exit code: 0 the window is up (or another one was brought to the front), 1 it could not be started.
// For tools\launcher_selftest.ps1: with the environment variable MK9R_LAUNCHER_TEST set, what a message box would
// say is written to the standard output instead, and no "Starting" card is shown.

#include <windows.h>

#include <string>

namespace {
constexpr const wchar_t* kTitle = L"MK9 Rollback";
constexpr const wchar_t* kWindowMutex = L"Local\\MK9RollbackLauncher";        // tools\netplay_gui.ps1 owns it
constexpr const wchar_t* kShowEvent = L"Local\\MK9RollbackLauncherShow";      // set: the open window shows itself
constexpr const wchar_t* kStartingMutex = L"Local\\MK9RollbackLauncherStarting";
constexpr const wchar_t* kSplashClass = L"MK9RollbackStarting";
constexpr DWORD kWaitForWindowMs = 45000;  // a cold PowerShell start behind an antivirus scan can take this long
constexpr int kIconResource = 1;

// The window's colours (tools\launcher_lib.ps1 $script:UiColors).
constexpr COLORREF kBg = RGB(0x0F, 0x11, 0x14);
constexpr COLORREF kBorder = RGB(0x2B, 0x30, 0x38);
constexpr COLORREF kText = RGB(0xE8, 0xEA, 0xED);
constexpr COLORREF kMuted = RGB(0x9A, 0xA3, 0xAF);
constexpr COLORREF kAccent = RGB(0xF2, 0x6B, 0x1D);
constexpr COLORREF kTrack = RGB(0x1C, 0x20, 0x26);

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// tools\netplay_gui.ps1 next to this program, or two folders up; empty when neither is there.
std::wstring FindScript(const std::wstring& exeDir) {
    const std::wstring here = exeDir + L"\\tools\\netplay_gui.ps1";
    if (FileExists(here)) return here;
    const std::wstring up = ParentOf(ParentOf(exeDir));
    if (!up.empty() && FileExists(up + L"\\tools\\netplay_gui.ps1")) return up + L"\\tools\\netplay_gui.ps1";
    return std::wstring();
}

// Windows PowerShell by its full path (never through PATH). From this 32-bit program the 64-bit one is reached
// through Sysnative: the window has always run in the 64-bit PowerShell ("Play Online.bat" started it from cmd).
std::wstring PowerShellPath() {
    wchar_t win[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring root = (n && n < MAX_PATH) ? std::wstring(win, n) : std::wstring(L"C:\\Windows");
    const wchar_t* tail = L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    if (wow64 && FileExists(root + L"\\Sysnative" + tail)) return root + L"\\Sysnative" + tail;
    return root + L"\\System32" + tail;
}

// What follows the program's own name on the command line, as typed.
const wchar_t* ArgumentsAsTyped() {
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') {
        for (++p; *p && *p != L'"'; ++p) {}
        if (*p == L'"') ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

std::wstring LastErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring text = n ? std::wstring(msg, n) : std::wstring(L"error ") + std::to_wstring(err);
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

bool g_test = false;  // MK9R_LAUNCHER_TEST
// A self-test uses names of its own (tools\netplay_gui.ps1 does the same): it neither meets nor disturbs the
// player's open window.
std::wstring Named(const wchar_t* base) { return std::wstring(base) + (g_test ? L"Test" : L""); }

void Tell(const std::wstring& text, UINT icon) {
    if (g_test) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], n, nullptr, nullptr);
        utf8 += "\r\n";
        DWORD wrote = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
        return;
    }
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | MB_SETFOREGROUND | icon);
}

// ---- the "Starting" card ----
struct Splash {
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    HFONT titleFont = nullptr;
    HFONT textFont = nullptr;
    int scale = 96;  // the screen's DPI
    int tick = 0;
};
Splash g_splash;

int Px(int at96) { return MulDiv(at96, g_splash.scale, 96); }

void PaintSplash(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC screen = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HBRUSH border = CreateSolidBrush(kBorder);
    FrameRect(dc, &rc, border);
    DeleteObject(border);

    const int iconSize = Px(56);
    const int iconX = Px(26), iconY = (rc.bottom - Px(4) - iconSize) / 2;
    if (g_splash.icon) DrawIconEx(dc, iconX, iconY, g_splash.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);

    SetBkMode(dc, TRANSPARENT);
    const int textX = iconX + iconSize + Px(18);
    HGDIOBJ oldFont = SelectObject(dc, g_splash.titleFont);
    SetTextColor(dc, kText);
    RECT title{textX, iconY + Px(2), rc.right - Px(16), iconY + Px(32)};
    DrawTextW(dc, L"MK9 Rollback", -1, &title, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, g_splash.textFont);
    SetTextColor(dc, kMuted);
    RECT sub{textX, iconY + Px(33), rc.right - Px(16), iconY + Px(56)};
    DrawTextW(dc, L"Starting\x2026", -1, &sub, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);

    // A bar that keeps moving along the bottom edge: the start takes a few seconds and has no percentage.
    const int trackH = Px(3);
    RECT track{1, rc.bottom - trackH - 1, rc.right - 1, rc.bottom - 1};
    HBRUSH trackBrush = CreateSolidBrush(kTrack);
    FillRect(dc, &track, trackBrush);
    DeleteObject(trackBrush);
    const int span = track.right - track.left;
    const int barW = span / 4;
    const int pos = (g_splash.tick * Px(6)) % (span + barW) - barW;
    RECT bar{track.left + (pos < 0 ? 0 : pos), track.top, track.left + pos + barW, track.bottom};
    if (bar.right > track.right) bar.right = track.right;
    if (bar.right > bar.left) {
        HBRUSH accent = CreateSolidBrush(kAccent);
        FillRect(dc, &bar, accent);
        DeleteObject(accent);
    }

    BitBlt(screen, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            PaintSplash(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            ++g_splash.tick;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void ShowSplash(HINSTANCE inst) {
    HDC screen = GetDC(nullptr);
    g_splash.scale = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (g_splash.scale < 96) g_splash.scale = 96;

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = SplashProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(kIconResource));
    wc.lpszClassName = kSplashClass;
    if (!RegisterClassExW(&wc)) return;

    g_splash.icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON, Px(56), Px(56), LR_DEFAULTCOLOR));
    g_splash.titleFont = CreateFontW(-Px(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_splash.textFont = CreateFontW(-Px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    const int w = Px(340), h = Px(112);
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    // No taskbar button of its own: the window that follows brings one.
    g_splash.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kSplashClass, kTitle, WS_POPUP, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_splash.hwnd) return;
    SetTimer(g_splash.hwnd, 1, 30, nullptr);
    ShowWindow(g_splash.hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_splash.hwnd);
}

void CloseSplash() {
    if (g_splash.hwnd) DestroyWindow(g_splash.hwnd);
    g_splash.hwnd = nullptr;
    if (g_splash.icon) DestroyIcon(g_splash.icon);
    if (g_splash.titleFont) DeleteObject(g_splash.titleFont);
    if (g_splash.textFont) DeleteObject(g_splash.textFont);
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---- the window's process ----
struct FindVisible {
    DWORD pid;
    bool found;
};

BOOL CALLBACK FindVisibleProc(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<FindVisible*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || !IsWindowVisible(hwnd)) return TRUE;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    if (rc.right - rc.left < 50 || rc.bottom - rc.top < 50) return TRUE;
    f->found = true;
    return FALSE;
}

// The window itself, or a message box of its script: either way the player has something to look at.
bool HasVisibleWindow(DWORD pid) {
    FindVisible f{pid, false};
    EnumWindows(FindVisibleProc, reinterpret_cast<LPARAM>(&f));
    return f.found;
}

// What PowerShell printed before it ended (its own error text when it refuses to run the script), at most `limit`
// characters. PowerShell without a console writes in the system's ANSI code page.
std::wstring ReadCaptured(HANDLE file, size_t limit) {
    if (file == INVALID_HANDLE_VALUE) return std::wstring();
    std::string bytes;
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    char buf[2048];
    DWORD got = 0;
    while (bytes.size() < 16384 && ReadFile(file, buf, sizeof buf, &got, nullptr) && got) bytes.append(buf, got);
    if (bytes.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring text(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), &text[0], n);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    size_t first = 0;
    while (first < text.size() && (text[first] == L'\r' || text[first] == L'\n' || text[first] == L' ')) ++first;
    text.erase(0, first);
    if (text.size() > limit) text = text.substr(0, limit) + L"\x2026";
    return text;
}

// True when a window already owns kWindowMutex and was asked to show itself. An older version's window has no
// kShowEvent: then false, and its script says "already open" itself.
bool HandOverToOpenWindow() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, Named(kWindowMutex).c_str());
    if (!m) return false;
    const DWORD w = WaitForSingleObject(m, 0);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
        // Nobody owns it: a window that is closing (it restarts through this program after an update).
        ReleaseMutex(m);
        CloseHandle(m);
        return false;
    }
    CloseHandle(m);
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, Named(kShowEvent).c_str());
    if (!e) return false;
    AllowSetForegroundWindow(ASFW_ANY);
    SetEvent(e);
    CloseHandle(e);
    return true;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // MK9R_LAUNCHER_TEST=<anything>: a self-test (no card, messages to stdout, names of its own); =splash: with the card.
    wchar_t testMode[16] = L"";
    g_test = GetEnvironmentVariableW(L"MK9R_LAUNCHER_TEST", testMode, 16) != 0;
    const bool withSplash = !g_test || !lstrcmpW(testMode, L"splash");
    const std::wstring exe = ExePath();
    const std::wstring script = FindScript(ParentOf(exe));
    if (script.empty()) {
        // Opened from inside the zip, Explorer extracts this file alone to a temporary folder.
        Tell(L"MK9 Rollback's other files are not next to this program.\n\n"
             L"Extract the whole zip first: right-click the zip, choose \"Extract All\", "
             L"then open \"MK9 Rollback.exe\" in the extracted folder.",
             MB_ICONINFORMATION);
        return 1;
    }
    if (HandOverToOpenWindow()) return 0;
    HANDLE starting = CreateMutexW(nullptr, FALSE, Named(kStartingMutex).c_str());
    if (starting && WaitForSingleObject(starting, 0) == WAIT_TIMEOUT) return 0;  // a start is already under way

    if (withSplash) ShowSplash(inst);
    PumpMessages();

    // PowerShell's own output goes to a temporary file that removes itself; stdin is empty.
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    wchar_t temp[MAX_PATH];
    HANDLE captured = INVALID_HANDLE_VALUE;
    if (const DWORD n = GetTempPathW(MAX_PATH, temp); n && n < MAX_PATH) {
        const std::wstring file = std::wstring(temp, n) + L"MK9Rollback-start-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
        captured = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

    SetEnvironmentVariableW(L"MK9R_LAUNCHER", exe.c_str());  // the window restarts through this program after an update
    const std::wstring ps = PowerShellPath();
    std::wstring cmd = L"\"" + ps + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
    if (const wchar_t* rest = ArgumentsAsTyped(); *rest) cmd += std::wstring(L" ") + rest;

    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = captured != INVALID_HANDLE_VALUE ? captured : nul;
    si.hStdError = si.hStdOutput;
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW: no console at all (and no "start hidden" request, which would hide the window too).
    if (!CreateProcessW(ps.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, ParentOf(script).c_str(), &si, &pi)) {
        const DWORD err = GetLastError();
        CloseSplash();
        Tell(L"MK9 Rollback could not start Windows PowerShell, which runs its window.\n\n" + LastErrorText(err) + L"\n(" + ps + L")\n\n"
             L"A rule set by whoever manages this PC (a company, a school, or security software) may forbid it. "
             L"Ask them to allow it, or play from another PC.",
             MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    AllowSetForegroundWindow(pi.dwProcessId);

    const DWORD began = GetTickCount();
    bool exited = false;
    for (;;) {
        const DWORD w = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 60, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) {
            exited = true;
            break;
        }
        PumpMessages();
        if (HasVisibleWindow(pi.dwProcessId)) break;
        if (GetTickCount() - began > kWaitForWindowMs) break;
    }
    CloseSplash();
    PumpMessages();

    int rc = 0;
    if (exited) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        // Nothing printed: the script told the player itself (its own error box), or another window was already open.
        const std::wstring said = code ? ReadCaptured(captured, 900) : std::wstring();
        if (code && !said.empty()) {
            Tell(L"MK9 Rollback could not open its window. Windows PowerShell said:\n\n" + said + L"\n\n"
                 L"The usual cause is a rule set by whoever manages this PC (a company, a school, or security software) "
                 L"that forbids PowerShell scripts. Ask them to allow the scripts in the \"tools\" folder next to this program, "
                 L"or play from another PC.",
                 MB_ICONERROR);
        }
        rc = code ? 1 : 0;
    }
    CloseHandle(pi.hProcess);
    if (captured != INVALID_HANDLE_VALUE) CloseHandle(captured);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (starting) {
        ReleaseMutex(starting);
        CloseHandle(starting);
    }
    return rc;
}
