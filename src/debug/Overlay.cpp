#include "Overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

#include "../hook/CodePatch.h"
#include "../hook/ObjectHooks.h"
#include "../rollback/FramePace.h"
#include "../rollback/RollbackManager.h"
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
volatile LONG gDraws = 0, gResimDraws = 0, gMaxUs = 0;
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

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
};
constexpr int kMaxV = 24000;
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

// The line for the current stats; colour by state.
void Format(const Overlay::Stats& s, int level, char* out, size_t n, DWORD* color) {
    *color = 0xFFFFFFFF;
    switch (s.mode) {
    case Overlay::kRollback:
    case Overlay::kLockstep: {
        const bool lock = s.mode == Overlay::kLockstep;
        int k = snprintf(out, n, "%s  ping %d ms  delay %d", lock ? "LOCKSTEP" : "ROLLBACK", s.ping, s.delay);
        if (!lock && k > 0 && (size_t)k < n)
            k += snprintf(out + k, n - k, "  rolled back: last %u, max %u (1 s), %u/s", s.lastDepth, s.maxDepth, s.loadsPerSec);
        if (k > 0 && (size_t)k < n) k += snprintf(out + k, n - k, "  behind %d", s.behindLocal);
        if (level >= 2 && k > 0 && (size_t)k < n)
            k += snprintf(out + k, n - k, "  | %d loads, %d replayed, %d kbps, queue %d, waits %d", s.loads, s.replayed, s.kbps, s.sendQueue,
                          s.barrierWaits);
        if (s.mismatched > 0 && k > 0 && (size_t)k < n) {
            snprintf(out + k, n - k, "  DESYNC (%d frames)", s.mismatched);
            *color = 0xFFFF4040;
        } else {
            *color = lock ? 0xFFFFE060 : 0xFF60FF60;
        }
        break;
    }
    case Overlay::kWaiting:
        snprintf(out, n, "ROLLBACK: waiting for the other player");
        *color = 0xFFFFE060;
        break;
    case Overlay::kSyncTest:
        snprintf(out, n, "SYNCTEST  %d loads, %d replayed", s.loads, s.replayed);
        break;
    case Overlay::kStopped:
        snprintf(out, n, "ROLLBACK STOPPED: %s", s.stopWhy);
        *color = 0xFFFF4040;
        break;
    case Overlay::kInfo:  // an online fight on MK9's own netcode, with the reason (Online::StatusText)
        snprintf(out, n, "%s", s.stopWhy);
        *color = 0xFFFFE060;
        break;
    default:
        snprintf(out, n, "ROLLBACK OFF");
        *color = 0xFFC0C0C0;
        break;
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

void Draw(IDirect3DDevice9* dev) {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    Overlay::Stats s = gBuf[gFront & 1];
    s.stopWhy[sizeof s.stopWhy - 1] = 0;
    const int level = gLevel;
    if (level <= 0 || (level == 1 && s.mode == Overlay::kOff)) return;
    char line[256];
    DWORD color;
    Format(s, level, line, sizeof line, &color);
    D3DVIEWPORT9 vp{};
    if (FAILED(dev->GetViewport(&vp)) || vp.Width < 64 || vp.Height < 64) return;
    const float scale = vp.Height >= 1000 ? 2.0f : 1.0f;
    const float x = (float)vp.X + 8 * scale, y = (float)vp.Y + 6 * scale;
    gN = 0;
    Quad(x - 4 * scale, y - 2 * scale, x + (float)strlen(line) * kCW * scale + 4 * scale, y + (kCH + 1) * scale, 0xA0000000);
    Text(line, x + scale, y + scale, scale, 0xFF000000);  // shadow
    Text(line, x, y, scale, color);
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
    if (gN >= 3) dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, gN / 3, gV, sizeof(Vtx));
    sb->Apply();
    sb->Release();
    InterlockedIncrement(&gDraws);
    if (Rollback::gIsResimulating) InterlockedIncrement(&gResimDraws);  // racy read: diagnostic only
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    const LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / (fq.QuadPart ? fq.QuadPart : 1));
    if (us > gMaxUs) InterlockedExchange(&gMaxUs, us);
}
}  // namespace

extern "C" void __cdecl MK9R_OverlayDraw(IDirect3DDevice9* dev) {
    InterlockedIncrement(&gInFlight);
    FramePace::OnPresent();  // presented-frame intervals (PACE lines), drawn or not
    if (gEnabled && dev) {
        if (!InterlockedExchange(&gFirstLogged, 1)) FirstCall(dev);
        if (gEnabled) Draw(dev);
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

void SetLevel(int level) { InterlockedExchange(&gLevel, level < 0 ? 0 : level > 2 ? 2 : level); }
int Level() { return gLevel; }

void Publish(const Stats& s) {
    const LONG back = (gFront & 1) ^ 1;
    gBuf[back] = s;
    InterlockedExchange(&gFront, back);
}

void LogCounters() {
    Logger::Info("OVERLAY draws %ld, while resimulating %ld, max draw time %ld us, level %ld%s", gDraws, gResimDraws, gMaxUs, gLevel,
                 gEnabled ? "" : " (disabled)");
}

}  // namespace Overlay
