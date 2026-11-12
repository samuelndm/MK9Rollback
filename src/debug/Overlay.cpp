#include "Overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <algorithm>
#include <cstring>

#include "../common/StrAppend.h"
#include "../hook/CodePatch.h"
#include "../hook/ObjectHooks.h"
#include "../rollback/FramePace.h"
#include "../rollback/RollbackManager.h"
#include "Hud.h"
#include "Logger.h"

// Site (docs\status_overlay_plan.md 2.3, checked against the disassembly 2026-10-29): RHIEndDrawingViewport 0x00666F20,
// presenting branch, after the state reset (render target 0 = the back buffer [0xE84538], depth, shaders, streams,
// indices and textures unbound) and right before EndScene:
//   0x00667106 A1 34 45 E8 00        mov eax, [0x00E84534]   (the IDirect3DDevice9Ex*)
//   0x0066710B 8B 08 8B 91 A8 00 00 00 50 FF D2  mov ecx,[eax]; mov edx,[ecx+0xA8]; push eax; call edx  (EndScene)
// The first instruction becomes a call to MK9R_OverlayStub, which draws and then performs it.
namespace {
constexpr uintptr_t kSite = 0x00667106;  // the stub reads the device from 0x00E84534 itself
const uint8_t kSiteBytes[16] = {0xA1, 0x34, 0x45, 0xE8, 0x00, 0x8B, 0x08, 0x8B, 0x91, 0xA8, 0x00, 0x00, 0x00, 0x50, 0xFF, 0xD2};

volatile LONG gEnabled = 0;   // the stub draws (else it only performs the replaced instruction)
volatile LONG gWanted = 0;    // Install succeeded once and Disable was not called: EnsureInstalled may re-apply
volatile LONG gInFlight = 0;
volatile LONG gLevel = 1;
volatile LONG gDemo = 0;      // 'huddemo N': the HUD display shows canned state N (Hud::Demo) instead of the published one
volatile LONG gDraws = 0, gResimDraws = 0, gMaxUs = 0;
volatile LONG gHudDraws = 0, gHudMaxVerts = 0, gHudDropped = 0;
volatile LONG gWinDraws = 0, gWinSumUs = 0, gWinMaxUs = 0;  // since the last LogCounters (at most a few thousand draws)
volatile LONG gFirstLogged = 0;

Overlay::Stats gBuf[2];
volatile LONG gFront = 0;

// Glyphs 0x20..0x7E, 8 x 14 cells, bit 7 = leftmost pixel: rendered once at install with GDI (no font data in the
// source, nothing created on the device).
constexpr int kCW = 8, kCH = 14;
uint8_t gGlyph[95][kCH];

bool BuildGlyphs() {
    const int w = 95 * kCW, h = kCH;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(ANSI_FIXED_FONT));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);
    memset(bits, 0, (size_t)w * h * 4);
    for (int c = 0; c < 95; ++c) {
        const wchar_t ch = (wchar_t)(0x20 + c);
        RECT r{c * kCW, 0, (c + 1) * kCW, h};
        ExtTextOutW(dc, c * kCW, 1, ETO_CLIPPED | ETO_OPAQUE, &r, &ch, 1, nullptr);
    }
    GdiFlush();
    const uint32_t* px = static_cast<const uint32_t*>(bits);
    int lit = 0;
    for (int c = 0; c < 95; ++c)
        for (int y = 0; y < kCH; ++y) {
            uint8_t row = 0;
            for (int x = 0; x < kCW; ++x)
                if ((px[y * w + c * kCW + x] & 0xFF) > 0x80) row |= (uint8_t)(0x80 >> x);
            gGlyph[c][y] = row;
            lit += row != 0;
        }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    if (font) DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
    return lit > 95;  // most glyphs have pixels
}

using Vtx = Hud::Vtx;  // x, y, z, rhw, colour
// The HUD display's largest frame in tests\host\hud_test.cpp is about half of this (two long notes at 1440 lines).
constexpr int kMaxV = 96000;
Vtx gV[kMaxV];  // render thread only
int gN = 0;

void Quad(float x0, float y0, float x1, float y1, DWORD c) {
    if (gN + 6 > kMaxV) return;
    x0 -= 0.5f, y0 -= 0.5f, x1 -= 0.5f, y1 -= 0.5f;
    const Vtx a{x0, y0, 0, 1, c}, b{x1, y0, 0, 1, c}, d{x0, y1, 0, 1, c}, e{x1, y1, 0, 1, c};
    gV[gN++] = a, gV[gN++] = b, gV[gN++] = d;
    gV[gN++] = b, gV[gN++] = e, gV[gN++] = d;
}

// Text as merged horizontal pixel runs. Returns the width drawn.
float Text(const char* s, float x, float y, float scale, DWORD c) {
    const float x0 = x;
    for (; *s; ++s, x += kCW * scale) {
        const int g = (unsigned char)*s - 0x20;
        if (g < 0 || g >= 95) continue;
        for (int row = 0; row < kCH; ++row) {
            const uint8_t bits = gGlyph[g][row];
            for (int b = 0; b < kCW;) {
                if (!(bits & (0x80 >> b))) {
                    ++b;
                    continue;
                }
                int e = b;
                while (e < kCW && (bits & (0x80 >> e))) ++e;
                Quad(x + b * scale, y + row * scale, x + e * scale, y + (row + 1) * scale, c);
                b = e;
            }
        }
    }
    return x - x0;
}

constexpr DWORD kWhite = 0xFFF2F2F2, kGreen = 0xFF40E060, kYellow = 0xFFFFD040, kRed = 0xFFFF4040, kGrey = 0xFFC0C0C0, kDim = 0xFF505050;

// The connection quality rule and the stop reasons' player texts are shared with the HUD display: Hud.cpp.
using Hud::Quality;
using Hud::StopText;

// The main line for the current stats. a: state text in color ca; b: an explicit suffix (DESYNC), drawn red, else empty.
// bars: connection quality marker (3 = good .. 1 = bad), -1 none.
void Format(const Overlay::Stats& s, int level, char* a, char* b, size_t n, DWORD* ca, int* bars) {
    *ca = kWhite;
    a[0] = b[0] = 0;
    *bars = -1;
    switch (s.mode) {
    case Overlay::kRollback:
    case Overlay::kLockstep: {
        const bool lock = s.mode == Overlay::kLockstep;
        const int rtt = s.rttMs >= 0 ? s.rttMs : s.ping;
        int k = snprintf(a, n, "%s", lock ? "LOCKSTEP" : "ROLLBACK");
        if (level >= 2) {
            if (s.rttMs >= 0 && k > 0 && (size_t)k < n) k = AppendF(a, n, k, "  rtt %d ms", s.rttMs);
            if (s.ping >= 0 && k > 0 && (size_t)k < n) k = AppendF(a, n, k, "  ping %d ms", s.ping);
            if (k > 0 && (size_t)k < n) k = AppendF(a, n, k, "  delay %d", s.delay);
            if (!lock && k > 0 && (size_t)k < n)
                k = AppendF(a, n, k, "  rolled back: last %u, max %u (1 s), %u/s", s.lastDepth, s.maxDepth, s.loadsPerSec);
            if (k > 0 && (size_t)k < n) k = AppendF(a, n, k, "  behind %d", s.behindLocal);
            if (k > 0 && (size_t)k < n)
                snprintf(a + k, n - k, "  | %d loads, %d replayed, %d kbps, queue %d, waits %d", s.loads, s.replayed, s.kbps, s.sendQueue,
                         s.barrierWaits);
        } else {
            if (rtt >= 0 && k > 0 && (size_t)k < n) k = AppendF(a, n, k, "  %d ms", rtt);
            if (k > 0 && (size_t)k < n) k = AppendF(a, n, k, "  delay %d", s.delay);
        }
        // Wi-Fi is the first thing to fix in an uneven match (other fighting games show it per player for that reason).
        static const char* const kWifi[4] = {"", "  Wi-Fi: you", "  Wi-Fi: opponent", "  Wi-Fi: both"};
        if (k > 0 && (size_t)k < n) k = AppendF(a, n, k, "%s", kWifi[s.wifi & 3]);
        if (level >= 2 && s.relayed && k > 0 && (size_t)k < n) snprintf(a + k, n - k, "  (Steam relay)");
        *bars = 3 - Quality(s);
        if (s.mismatched > 0) snprintf(b, n, "  DESYNC (%d frames)", s.mismatched);
        break;
    }
    case Overlay::kWaiting:
        snprintf(a, n, "ROLLBACK: waiting for the other player");
        *ca = 0xFFFFE890;
        break;
    case Overlay::kSyncTest:
        snprintf(a, n, "SYNCTEST  %d loads, %d replayed", s.loads, s.replayed);
        break;
    case Overlay::kStopped:
        snprintf(a, n, "ROLLBACK STOPPED: %s", StopText(s.stopWhy));
        *ca = kRed;
        break;
    case Overlay::kInfo:  // an online fight on MK9's own netcode, with the reason (Online::StatusText)
        snprintf(a, n, "%s", s.stopWhy);
        *ca = 0xFFFFE890;
        break;
    default:
        snprintf(a, n, "ROLLBACK OFF");
        *ca = kGrey;
        break;
    }
}

// F9 switches compact <-> detailed (level 1 <-> 2); level 0 (off) is left alone. Render thread; gLevel is also set by
// the command channel, so the switch is a compare-exchange.
bool gHintDone = false;
bool gF9Prev = false;
void PollToggle() {
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool edge = down && !gF9Prev;
    gF9Prev = down;
    if (!edge) return;
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) return;
    if (InterlockedCompareExchange(&gLevel, 2, 1) != 1) InterlockedCompareExchange(&gLevel, 1, 2);
    gHintDone = true;
}

float TextS(const char* s, float x, float y, float scale, DWORD c) {
    Text(s, x + scale, y + scale, scale, 0xFF000000);  // shadow
    return Text(s, x, y, scale, c);
}

// One boxed line, cut to end before `right`. a, b are cut in place. bars >= 0: quality marker after the text.
void DrawLine(char* a, DWORD ca, char* b, int bars, float x, float y, float scale, float right) {
    const float barW = bars >= 0 ? 22 * scale : 0;
    const int maxChars = (int)((right - x - barW - 8 * scale) / (kCW * scale));
    if (maxChars < 1) return;
    int la = (int)strlen(a), lb = b ? (int)strlen(b) : 0;
    if (la > maxChars) a[maxChars] = 0, la = maxChars, lb = 0;
    else if (la + lb > maxChars) b[maxChars - la] = 0, lb = maxChars - la;
    const float tw = (float)(la + lb) * kCW * scale;
    Quad(x - 4 * scale, y - 2 * scale, x + tw + barW + 4 * scale, y + (kCH + 1) * scale, 0xA0000000);
    TextS(a, x, y, scale, ca);
    if (lb) TextS(b, x + (float)la * kCW * scale, y, scale, kRed);
    if (bars >= 0) {
        const DWORD c = bars >= 3 ? kGreen : bars == 2 ? kYellow : kRed;
        const float bx = x + tw + 6 * scale, by = y + kCH * scale;
        for (int i = 0; i < 3; ++i) {
            const float h = (float)(5 + 4 * i) * scale, l = bx + (float)i * 6 * scale;
            Quad(l, by - h, l + 4 * scale, by, i < bars ? c : kDim);
        }
    }
}


bool Executable(const void* p) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    return mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

// Every IDirect3DDevice9 vtable entry (119) must point into committed executable memory; name: the module
// holding the vtable, or "heap" (2026-10-29 this PC's device vtable was a heap copy next to the device, as an
// overlay or hooking layer makes; calls through it worked). Calls through the interface are valid on any
// implementation; entries that are not code mean this is not a device.
bool VtableModule(IDirect3DDevice9* dev, char* name, int n) {
    const auto* vt = *reinterpret_cast<void* const* const*>(dev);
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(vt, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        snprintf(name, n, "unreadable memory");
        return false;
    }
    HMODULE m = nullptr;
    char path[MAX_PATH] = "heap";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(vt), &m) && m)
        GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    snprintf(name, n, "%s", base ? base + 1 : path);
    for (int i = 0; i < 119; i++)
        if (!Executable(vt[i])) {
            snprintf(name, n, "%s, entry %d not code", base ? base + 1 : path, i);
            return false;
        }
    return true;
}

void FirstCall(IDirect3DDevice9* dev) {
    char mod[64];
    if (!VtableModule(dev, mod, sizeof mod)) {  // no calls through it
        InterlockedExchange(&gEnabled, 0);
        Logger::Info("OVERLAY disabled: the device's vtable is not a device's (%s)", mod);
        return;
    }
    IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
    dev->GetRenderTarget(0, &rt);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    Logger::Info("OVERLAY first draw: device %08X, vtable %08X in %s, render target 0 %s the back buffer, viewport %lux%lu, render thread %lu "
                 "(game thread %lu)",
                 (unsigned)reinterpret_cast<uintptr_t>(dev), (unsigned)*reinterpret_cast<const uintptr_t*>(dev), mod,
                 rt && rt == bb ? "is" : "is NOT", vp.Width, vp.Height,
                 GetCurrentThreadId(), ObjectHooks::gGameThread);
    if (rt) rt->Release();
    if (bb) bb->Release();
}

// How the picture reaches the screen (round 3; once at the first draw and at each session's start): the swap chain's
// present parameters and the refresh rate of the monitor the window is on. A match that "stutters with a good
// connection" is often this: vertical sync against a 59.94 or 75 Hz refresh, or a 144 Hz monitor in windowed mode.
volatile LONG gDisplayLogWanted = 1;
void LogDisplay(IDirect3DDevice9* dev) {
    IDirect3DSwapChain9* chain = nullptr;
    if (FAILED(dev->GetSwapChain(0, &chain)) || !chain) return;
    D3DPRESENT_PARAMETERS pp{};
    const HRESULT hr = chain->GetPresentParameters(&pp);
    chain->Release();
    if (FAILED(hr)) return;
    unsigned hz = 0;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    const HMONITOR mon = pp.hDeviceWindow ? MonitorFromWindow(pp.hDeviceWindow, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hz = dm.dmDisplayFrequency;
    const bool vsync = pp.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
    const char* note = "";
    if (vsync && hz && (hz < 60 || hz > 61))
        note = hz == 59 ? "; note: vertical sync at 59.94 Hz shows one frame twice every ~17 s (set the monitor to 60 Hz)"
                        : "; note: vertical sync on a monitor that is not at 60 Hz makes the 60 fps game uneven (set it to 60 Hz, or turn vertical sync off)";
    Logger::Info("DISPLAY: %s %ux%u, vertical sync %s (presentation interval %08X), swap effect %d, %u back buffer(s), fullscreen "
                 "refresh %u Hz; the window's monitor runs at %u Hz%s",
                 pp.Windowed ? "windowed" : "fullscreen", pp.BackBufferWidth, pp.BackBufferHeight, vsync ? "on" : "off",
                 (unsigned)pp.PresentationInterval, (int)pp.SwapEffect, pp.BackBufferCount, pp.FullScreen_RefreshRateInHz, hz, note);
}

void Draw(IDirect3DDevice9* dev) {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    PollToggle();
    Overlay::Stats s = gBuf[gFront & 1];
    s.stopWhy[sizeof s.stopWhy - 1] = 0;
    int level = gLevel;
    const int demo = gDemo;
    if (demo > 0 && Hud::Demo(demo, &s) && level <= 0) level = 1;
    D3DVIEWPORT9 vp{};
    if (FAILED(dev->GetViewport(&vp)) || vp.Width < 64 || vp.Height < 64) return;
    // The faces of the display that belongs to the game's HUD (Hud.cpp) are built once per picture size: at the first
    // frame after the DLL loaded (the game's start) or after a resolution change, whatever the level is then, so that
    // the tens of milliseconds never fall into a fight (F9 can bring the display in at any moment).
    const Hud::View view{(int)vp.X, (int)vp.Y, (int)vp.Width, (int)vp.Height};
    static int builds = 0;
    bool wait = false;
    const bool faces = Hud::Prepare(view, &wait);
    if (Hud::Builds() != builds) {
        builds = Hud::Builds();
        LARGE_INTEGER tb, tf;
        QueryPerformanceCounter(&tb);
        QueryPerformanceFrequency(&tf);
        char text[200];
        Hud::Describe(text, sizeof text);
        Logger::Info("HUD display: faces %s, %.1f ms%s", text, (double)(tb.QuadPart - t0.QuadPart) * 1000.0 / (double)(tf.QuadPart ? tf.QuadPart : 1),
                     faces ? "" : "; the status line is drawn the old way");
        t0 = tb;  // the one-time build is not a draw time
    }
    if (level <= 0) return;
    if (level == 1 && wait) return;  // the picture size has just changed: nothing until the faces for it are built
    gN = 0;
    // Level 1: the HUD display. Without a usable face from Windows, the old line below is drawn instead.
    const bool hud = level == 1 && faces;
    if (hud) {
        gN = Hud::Build(s, view, GetTickCount64(), gV, kMaxV);
        if (gN > gHudMaxVerts) InterlockedExchange(&gHudMaxVerts, gN);
        if (const int dropped = Hud::Dropped()) InterlockedExchangeAdd(&gHudDropped, dropped);
        if (gN >= 3) InterlockedIncrement(&gHudDraws);
    } else {
        if (level == 1 && s.mode == Overlay::kOff) return;
        static char la[256], lb[256], lw[96];  // render thread only (Format writes la and lb with the same size)
        DWORD color;
        int bars;
        Format(s, level, la, lb, sizeof la, &color, &bars);
        const float scale = (float)std::max(1, (int)((double)vp.Height / 540.0 + 0.5));
        const float x = (float)vp.X + 8 * scale, y = (float)vp.Y + 6 * scale, right = (float)vp.X + (float)vp.Width - 8 * scale;
        const float step = (kCH + 4) * scale;
        const bool live = s.mode == Overlay::kRollback || s.mode == Overlay::kLockstep;
        float ly = y;
        DrawLine(la, color, lb, bars, x, ly, scale, right);
        ly += step;
        // Transient warnings (a few seconds after the event; the stats are published only while the game runs).
        DWORD wc = kYellow;
        lw[0] = 0;
        if (live) {
            if (s.peerInterrupted) {
                snprintf(lw, sizeof lw, "Opponent not responding...");
                wc = kRed;
            } else {
                const bool stall = s.netStallMs >= 150 && s.netStallAgoS <= 5;
                const bool hitch = s.localHitchMs >= 100 && s.localHitchAgoS <= 5;
                if (hitch && (!stall || s.localHitchMs >= s.netStallMs))
                    snprintf(lw, sizeof lw, "This PC hitched: %u ms frame", (unsigned)s.localHitchMs);
                else if (stall)
                    snprintf(lw, sizeof lw, "Connection hiccup: waited %.1f s for the opponent", s.netStallMs / 1000.0);
            }
        } else if (s.mode == Overlay::kStopped) {
            snprintf(lw, sizeof lw, "MK9 netcode plays the rest of this match");
            wc = kWhite;
        }
        if (lw[0]) {
            DrawLine(lw, wc, nullptr, -1, x, ly, scale, right);
            ly += step;
        }
        // Hint for the first ~8 s of the first session after the DLL loaded (only where the old line stands in for the
        // HUD display: the Play Online window names F9 beside its choice).
        static ULONGLONG hintStart = 0;
        if (live && !hintStart) hintStart = GetTickCount64();
        if (level == 1 && hintStart && !gHintDone) {
            if (GetTickCount64() - hintStart < 8000) {
                static char lh[] = "F9: network details";
                DrawLine(lh, kGrey, nullptr, -1, x, ly, scale, right);
            } else {
                gHintDone = true;
            }
        }
    }
    if (gN < 3) return;  // nothing on screen this frame: the device is not touched
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetTexture(0, nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, gN / 3, gV, sizeof(Vtx));
    sb->Apply();
    sb->Release();
    InterlockedIncrement(&gDraws);
    if (Rollback::gIsResimulating) InterlockedIncrement(&gResimDraws);  // racy read: diagnostic only
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    const LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / (fq.QuadPart ? fq.QuadPart : 1));
    if (us > gMaxUs) InterlockedExchange(&gMaxUs, us);
    InterlockedIncrement(&gWinDraws);
    InterlockedExchangeAdd(&gWinSumUs, us > 100000 ? 100000 : us);  // capped: the sum stays far inside a LONG
    if (us > gWinMaxUs) InterlockedExchange(&gWinMaxUs, us);
}
}  // namespace

namespace {
// ---- render latency (Overlay::SetFrameLatency) ----
// 2026-11-09: the player compared online MK9 with current games ("they feel offline"). Online adds the input delay;
// the game's own pipeline adds what D3D9 queues ahead of the GPU (D3D9Ex default: up to 3 frames) on top of UE3's
// render thread. The probe measures that queue; the cap trims it.
volatile LONG gLatencyWanted = 0;   // 0: leave the game's value
LONG gLatencyApplied = 0;           // render thread only
int gLatencyDefault = -1;
constexpr int kQueries = 8;
IDirect3DQuery9* gQuery[kQueries] = {};
bool gQueryPending[kQueries] = {};
int gQueryNext = 0;
bool gQueryFailed = false;
volatile LONG gQueueFrames = 0, gQueueSum = 0, gQueueMax = 0, gQueueFull = 0;

void ReleaseQueries() {
    for (int i = 0; i < kQueries; ++i) {
        if (gQuery[i]) gQuery[i]->Release();
        gQuery[i] = nullptr;
        gQueryPending[i] = false;
    }
}

void LatencyProbe(IDirect3DDevice9* dev) {
    const LONG want = gLatencyWanted;
    if (want != gLatencyApplied || gLatencyDefault < 0) {
        IDirect3DDevice9Ex* ex = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex))) && ex) {
            UINT cur = 0;
            ex->GetMaximumFrameLatency(&cur);
            if (gLatencyDefault < 0) {
                gLatencyDefault = (int)cur;
                Logger::Info("GPULATENCY: the device's maximum frame latency is %u (D3D9Ex)", cur);
            }
            if (want != gLatencyApplied) {
                const UINT set = want > 0 ? (UINT)want : (UINT)gLatencyDefault;
                const HRESULT hr = ex->SetMaximumFrameLatency(set);
                Logger::Info("GPULATENCY: maximum frame latency %u -> %u (%s)", cur, set, SUCCEEDED(hr) ? "ok" : "refused");
                gLatencyApplied = want;
            }
            ex->Release();
        } else if (gLatencyDefault < 0) {
            gLatencyDefault = 0;
            gLatencyApplied = want;
            Logger::Info("GPULATENCY: the device is not a D3D9Ex device; no frame latency control");
        }
    }
    if (gQueryFailed) return;
    // Frames whose query has not signalled yet: still queued or being drawn by the GPU.
    LONG queued = 0;
    for (int i = 0; i < kQueries; ++i) {
        if (!gQueryPending[i] || !gQuery[i]) continue;
        const HRESULT hr = gQuery[i]->GetData(nullptr, 0, 0);
        if (hr == S_OK) {
            gQueryPending[i] = false;
        } else if (hr == S_FALSE) {
            ++queued;
        } else {  // device lost / reset: start over
            ReleaseQueries();
            queued = 0;
            break;
        }
    }
    InterlockedIncrement(&gQueueFrames);
    InterlockedExchangeAdd(&gQueueSum, queued);
    if (queued > gQueueMax) InterlockedExchange(&gQueueMax, queued);
    IDirect3DQuery9*& q = gQuery[gQueryNext];
    if (gQueryPending[gQueryNext]) {  // all slots in flight: count it and reuse the oldest
        InterlockedIncrement(&gQueueFull);
        gQueryPending[gQueryNext] = false;
    }
    if (!q && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q))) {
        q = nullptr;
        gQueryFailed = true;
        Logger::Info("GPUQUEUE: event queries unavailable; no queue measurements");
        return;
    }
    if (SUCCEEDED(q->Issue(D3DISSUE_END))) gQueryPending[gQueryNext] = true;
    gQueryNext = (gQueryNext + 1) % kQueries;
}

// ---- 'hudshot <file>': the finished frame, with the display on it, written as a BMP ----
// For the test harness: the HUD display's place in a real fight, in the back buffer's own pixels, also while the PC is
// locked (a screenshot of the screen shows the lock screen then). The surfaces are created and released inside the one
// frame that takes the picture; nothing stays on the device.
volatile LONG gShotWanted = 0;
wchar_t gShotPath[MAX_PATH];  // the game thread writes it, then sets gShotWanted

void Capture(IDirect3DDevice9* dev) {
    IDirect3DSurface9 *rt = nullptr, *plain = nullptr, *sys = nullptr;
    const char* why = nullptr;
    D3DSURFACE_DESC d{};
    HRESULT hr = dev->GetRenderTarget(0, &rt);
    if (FAILED(hr) || !rt) why = "GetRenderTarget";
    else if (FAILED(hr = rt->GetDesc(&d))) why = "GetDesc";
    else if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) why = "the back buffer is not 32-bit BGRA";
    IDirect3DSurface9* src = rt;
    if (!why && d.MultiSampleType != D3DMULTISAMPLE_NONE) {  // a multisampled target is resolved first
        if (FAILED(hr = dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &plain, nullptr))) why = "CreateRenderTarget";
        else if (FAILED(hr = dev->StretchRect(rt, nullptr, plain, nullptr, D3DTEXF_NONE))) why = "StretchRect";
        src = plain;
    }
    if (!why && FAILED(hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) why = "CreateOffscreenPlainSurface";
    if (!why && FAILED(hr = dev->GetRenderTargetData(src, sys))) why = "GetRenderTargetData";
    D3DLOCKED_RECT lr{};
    if (!why && FAILED(hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) why = "LockRect";
    if (!why) {
        const HANDLE f = CreateFileW(gShotPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            why = "the file could not be created";
            hr = HRESULT_FROM_WIN32(GetLastError());
        } else {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof ih;
            ih.biWidth = (LONG)d.Width;
            ih.biHeight = (LONG)d.Height;  // bottom-up
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
            DWORD n = 0;
            WriteFile(f, &fh, sizeof fh, &n, nullptr);
            WriteFile(f, &ih, sizeof ih, &n, nullptr);
            for (UINT y = d.Height; y-- > 0;) WriteFile(f, static_cast<const uint8_t*>(lr.pBits) + (size_t)y * lr.Pitch, d.Width * 4, &n, nullptr);
            CloseHandle(f);
        }
        sys->UnlockRect();
    }
    if (sys) sys->Release();
    if (plain) plain->Release();
    if (rt) rt->Release();
    if (why) Logger::Info("HUDSHOT failed: %s (%08lX), back buffer %ux%u format %d multisample %d", why, (unsigned long)hr, d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType);
    else Logger::Info("HUDSHOT written: %ux%u, format %d, multisample %d", d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType);
}
}  // namespace

extern "C" void __cdecl MK9R_OverlayDraw(IDirect3DDevice9* dev) {
    InterlockedIncrement(&gInFlight);
    FramePace::OnPresent();  // presented-frame intervals (PACE lines), drawn or not
    if (dev && gWanted) LatencyProbe(dev);
    if (dev && gWanted && InterlockedExchange(&gDisplayLogWanted, 0)) LogDisplay(dev);
    if (gEnabled && dev) {
        if (!InterlockedExchange(&gFirstLogged, 1)) FirstCall(dev);
        if (gEnabled) Draw(dev);
        if (gEnabled && InterlockedExchange(&gShotWanted, 0)) Capture(dev);
    }
    InterlockedDecrement(&gInFlight);
}
// Reached by the E8 written at 0x00667106 ([esp] = 0x0066710B): draw with every register and the flags kept, then
// perform the replaced instruction (mov eax, [0x00E84534]) and return to the EndScene call.
asm(R"(
    .text
    .globl _MK9R_OverlayStub
_MK9R_OverlayStub:
    pushfl
    pushal
    movl 0x00E84534, %eax
    pushl %eax
    call _MK9R_OverlayDraw
    addl $4, %esp
    popal
    popfl
    movl 0x00E84534, %eax
    ret
)");
extern "C" void MK9R_OverlayStub();

namespace Overlay {

bool Install() {
    static bool glyphs = false;
    if (!glyphs) glyphs = BuildGlyphs();
    if (!glyphs) {
        Logger::Info("OVERLAY not installed: the glyphs could not be built");
        return false;
    }
    uint8_t call[5] = {0xE8};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_OverlayStub) - (kSite + 5));
    memcpy(call + 1, &rel, 4);
    if (!memcmp(reinterpret_cast<const void*>(kSite), call, 5)) return true;  // already in
    if (memcmp(reinterpret_cast<const void*>(kSite), kSiteBytes, sizeof kSiteBytes)) {
        Logger::Info("OVERLAY not installed: unexpected bytes at %08X", (unsigned)kSite);
        return false;
    }
    if (!CodePatch::Write(kSite, kSiteBytes, call, 5, "RHIEndDrawingViewport pre-EndScene 0x00667106 (overlay)")) return false;
    InterlockedExchange(&gEnabled, 1);
    InterlockedExchange(&gWanted, 1);
    return true;
}

void EnsureInstalled() {
    // the original instruction is back: a RestoreAll (e.g. a failed ObjectHooks install) removed the patch
    if (!gWanted || memcmp(reinterpret_cast<const void*>(kSite), kSiteBytes, 5) != 0) return;
    Logger::Info("OVERLAY: the patch was removed (CodePatch::RestoreAll); applying it again");
    Install();
}

void Disable() {
    InterlockedExchange(&gWanted, 0);
    InterlockedExchange(&gEnabled, 0);
}

long InFlight() { return gInFlight; }
void RequestDisplayLog() { InterlockedExchange(&gDisplayLogWanted, 1); }

void SetLevel(int level) { InterlockedExchange(&gLevel, level < 0 ? 0 : level > 2 ? 2 : level); }
int Level() { return gLevel; }
void SetDemo(int n) { InterlockedExchange(&gDemo, n < 0 || n > Hud::kDemos ? 0 : n); }
int Demo() { return gDemo; }

void RequestShot(const char* path) {
    if (gShotWanted) return;  // the previous one is not taken yet
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, gShotPath, MAX_PATH)) return;
    InterlockedExchange(&gShotWanted, 1);
}

void Publish(const Stats& s) {
    const LONG back = (gFront & 1) ^ 1;
    gBuf[back] = s;
    InterlockedExchange(&gFront, back);
}

void LogCounters() {
    // The draw time since the previous report (every ten seconds of a session, and at each 'huddemo'): the maximum over
    // the whole run says nothing about the usual cost, one slow frame at the start sets it for the evening.
    const LONG wn = InterlockedExchange(&gWinDraws, 0), wsum = InterlockedExchange(&gWinSumUs, 0);
    const LONG wmax = InterlockedExchange(&gWinMaxUs, 0);
    Logger::Info("OVERLAY draws %ld, while resimulating %ld, max draw time %ld us, level %ld%s; HUD display: %ld of them, largest "
                 "frame %ld vertices of %d, %ld rectangles dropped; since the last report %ld draws, average %ld us, max %ld us",
                 gDraws, gResimDraws, gMaxUs, gLevel, gEnabled ? "" : " (disabled)", gHudDraws, gHudMaxVerts, kMaxV, gHudDropped, wn,
                 wn ? wsum / wn : 0, wmax);
    const LONG n = InterlockedExchange(&gQueueFrames, 0), sum = InterlockedExchange(&gQueueSum, 0);
    const LONG mx = InterlockedExchange(&gQueueMax, 0), full = InterlockedExchange(&gQueueFull, 0);
    if (n)
        Logger::Info("GPUQUEUE: frames not yet finished by the GPU at EndScene: avg %.2f max %ld over %ld frames, ring full %ld "
                     "(maximum frame latency %ld, 0 = the game's %d)",
                     (double)sum / n, mx, n, full, gLatencyWanted, gLatencyDefault);
}

void SetFrameLatency(int n) { InterlockedExchange(&gLatencyWanted, n < 0 ? 0 : n > 16 ? 16 : n); }
int FrameLatencyWanted() { return (int)gLatencyWanted; }

}  // namespace Overlay
