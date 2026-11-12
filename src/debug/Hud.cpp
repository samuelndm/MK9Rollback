#include "Hud.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace {
using Hud::Vtx;
using Overlay::Stats;

// ---------------------------------------------------------------------------------------------------------------
// Faces: every glyph as two lists of rectangles, the glyph itself ("fill") and its dark edge.

struct Run {
    int16_t x0, x1, y0, y1;  // glyph pixels [x0, x1) x [y0, y1), from the pen (x) and the baseline (y, downwards)
    uint8_t a;               // coverage, 1..255
};
struct Glyph {
    int fill = 0, fillN = 0, edge = 0, edgeN = 0;  // into Face::runs
    int adv = 0;
};
constexpr int kFirstChar = 0x20, kChars = 95;
constexpr int kWifiGlyph = kChars, kGlyphs = kChars + 1;

struct Face {
    int px = 0;     // em height the glyphs were rendered at
    int capH = 0;   // height of a capital letter, in glyph pixels
    int reach = 1;  // width of the dark edge, in glyph pixels
    int zoom = 1;   // screen pixels per glyph pixel (2 above 1440 lines: bounds the rectangle count)
    Glyph g[kGlyphs];
    Run* runs = nullptr;
    int n = 0, cap = 0;
    int Em() const { return px * zoom; }
    int Cap() const { return capH * zoom; }
    int Reach() const { return reach * zoom; }
};

bool Push(Face& f, int x0, int x1, int y, int a) {
    if (f.n == f.cap) {
        const int cap = f.cap ? f.cap * 2 : 8192;
        Run* r = static_cast<Run*>(realloc(f.runs, (size_t)cap * sizeof(Run)));
        if (!r) return false;
        f.runs = r;
        f.cap = cap;
    }
    f.runs[f.n++] = Run{(int16_t)x0, (int16_t)x1, (int16_t)y, (int16_t)(y + 1), (uint8_t)a};
    return true;
}

// One list being built row by row: a run equal to one in the row above becomes part of that rectangle (stems and bars
// are most of a glyph, so this halves the count).
struct Layer {
    Face* f;
    int prev[96], prevN = 0, cur[96], curN = 0, row = 0;
    explicit Layer(Face* face) : f(face) {}
    void Row(int y) {
        memcpy(prev, cur, sizeof(int) * (size_t)curN);
        prevN = curN;
        curN = 0;
        row = y;
    }
    bool Add(int x0, int x1, int a) {
        for (int i = 0; i < prevN; ++i) {
            Run& r = f->runs[prev[i]];
            if (r.y1 == row && r.x0 == x0 && r.x1 == x1 && r.a == a) {
                r.y1 = (int16_t)(row + 1);
                if (curN < 96) cur[curN++] = prev[i];
                return true;
            }
        }
        if (!Push(*f, x0, x1, row, a)) return false;
        if (curN < 96) cur[curN++] = f->n - 1;
        return true;
    }
};

constexpr int kMaxSide = 112;                 // largest glyph bitmap side taken (a 48 px face stays far below)
uint8_t gCov[kMaxSide * kMaxSide];            // the glyph being built: coverage 0..255, w x h
uint8_t gRaw[kMaxSide * (kMaxSide + 4)];      // GetGlyphOutline's buffer (rows padded to 4 bytes, values 0..64)

// gCov (w x h, its top-left at ox, oy from the pen and the baseline) -> the glyph's two lists.
bool AddLayers(Face& f, Glyph& g, int w, int h, int ox, int oy) {
    g.fill = f.n;
    {
        Layer l(&f);
        for (int y = 0; y < h; ++y) {
            l.Row(oy + y);
            for (int x = 0; x < w;) {
                const uint8_t a = gCov[y * w + x];
                if (!a) {
                    ++x;
                    continue;
                }
                int e = x + 1;
                while (e < w && gCov[y * w + e] == a) ++e;
                if (!l.Add(ox + x, ox + e, a)) return false;
                x = e;
            }
        }
    }
    g.fillN = f.n - g.fill;
    // The dark edge: each pixel takes the largest coverage within reach, full up to `reach` pixels away and fading over
    // one more. Pixels the glyph covers completely are left out (the glyph is drawn over them anyway, and a fading
    // display must not darken the inside of its own letters).
    g.edge = f.n;
    const int R = f.reach + 1;
    float kern[9][9];
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx)
            kern[dy + R][dx + R] = std::clamp((float)f.reach + 1.0f - std::sqrt((float)(dx * dx + dy * dy)), 0.0f, 1.0f);
    Layer l(&f);
    for (int y = -R; y < h + R; ++y) {
        l.Row(oy + y);
        int start = -R, runA = 0;
        for (int x = -R; x <= w + R; ++x) {  // one past the end closes the last run
            int a = 0;
            if (x < w + R && !(x >= 0 && x < w && y >= 0 && y < h && gCov[y * w + x] == 255)) {
                float m = 0;
                for (int dy = -R; dy <= R; ++dy) {
                    const int sy = y + dy;
                    if (sy < 0 || sy >= h) continue;
                    for (int dx = -R; dx <= R; ++dx) {
                        const int sx = x + dx;
                        if (sx < 0 || sx >= w) continue;
                        m = std::max(m, (float)gCov[sy * w + sx] * kern[dy + R][dx + R]);
                    }
                }
                a = ((int)(m * 8.0f / 255.0f + 0.5f)) * 255 / 8;  // 8 levels: more equal neighbours, fewer rectangles
            }
            if (a != runA) {
                if (runA && !l.Add(ox + start, ox + x, runA)) return false;
                start = x;
                runA = a;
            }
        }
    }
    g.edgeN = f.n - g.edge;
    return true;
}

// The Wi-Fi mark, as tall as a capital letter: a dot and two or three arcs, from their distance functions.
bool AddWifi(Face& f) {
    const float H = (float)f.capH;
    const bool three = f.capH >= 16;
    const float t = std::max(1.5f, (three ? 0.13f : 0.17f) * H);  // stroke
    const float dot = std::max(1.0f, 0.13f * H);
    const float cy = H - dot;
    const float rOut = cy - t * 0.5f;
    const float radii[3] = {rOut, rOut - 2.0f * t, rOut - 4.0f * t};
    const int arcs = three ? 3 : 2;
    const float s = 0.70710678f;  // 45 degrees to each side
    const int half = (int)std::ceil(rOut * s + t * 0.5f);
    const int w = 2 * half + 1, h = f.capH;
    if (w > kMaxSide || h > kMaxSide || h < 4) return false;
    const float cx = (float)w * 0.5f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float px = std::fabs((float)x + 0.5f - cx), py = cy - ((float)y + 0.5f);
            const float len = std::sqrt(px * px + py * py);
            float d = len - dot;
            for (int i = 0; i < arcs; ++i) {
                const float ra = radii[i];
                const float arc = (s * px > s * py) ? std::sqrt((px - s * ra) * (px - s * ra) + (py - s * ra) * (py - s * ra))
                                                    : std::fabs(len - ra);
                d = std::min(d, arc - t * 0.5f);
            }
            const int q = (int)(std::clamp(0.5f - d, 0.0f, 1.0f) * 16.0f + 0.5f);
            gCov[y * w + x] = (uint8_t)(q * 255 / 16);
        }
    Glyph& g = f.g[kWifiGlyph];
    g.adv = w;
    return AddLayers(f, g, w, h, 0, -h);
}

// A condensed bold face reads as part of a fighting game's HUD. Bahnschrift ships with Windows 10 (1709) and later;
// the others are fallbacks every Windows has. A face counts only when Windows really selected it.
struct Pick {
    const wchar_t* name;
    int weight;
};
const Pick kPicks[] = {{L"Bahnschrift SemiBold Condensed", FW_DONTCARE}, {L"Bahnschrift Condensed", FW_BOLD},
                       {L"Arial Narrow", FW_BOLD},                       {L"Segoe UI Semibold", FW_DONTCARE},
                       {L"Tahoma", FW_BOLD},                             {L"Arial", FW_BOLD}};
wchar_t gFaceName[LF_FACESIZE] = L"";

HFONT SelectFace(HDC dc, int px, HGDIOBJ* old) {
    HFONT font = nullptr;
    for (const Pick& p : kPicks) {
        font = CreateFontW(-px, 0, 0, 0, p.weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, p.name);
        if (!font) continue;
        const HGDIOBJ was = SelectObject(dc, font);
        if (!*old) *old = was;
        wchar_t got[LF_FACESIZE] = L"";
        GetTextFaceW(dc, LF_FACESIZE, got);
        if (!lstrcmpiW(got, p.name) || &p == &kPicks[std::size(kPicks) - 1]) {
            lstrcpynW(gFaceName, got, LF_FACESIZE);
            return font;
        }
        SelectObject(dc, *old);
        DeleteObject(font);
        font = nullptr;
    }
    return font;
}

bool BuildFace(Face& f, HDC dc, int px, int zoom) {
    f.n = 0;
    f.px = px;
    f.zoom = zoom;
    f.reach = px >= 26 ? 2 : 1;
    f.capH = 0;
    for (Glyph& g : f.g) g = Glyph{};
    HGDIOBJ old = nullptr;
    HFONT font = SelectFace(dc, px, &old);
    if (!font) return false;
    static const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    bool ok = true;
    int drawn = 0;
    for (int c = 0; c < kChars && ok; ++c) {
        if (kFirstChar + c >= 'a' && kFirstChar + c <= 'z') continue;  // every text of the display is in capitals
        GLYPHMETRICS gm{};
        const DWORD size = GetGlyphOutlineW(dc, (UINT)(kFirstChar + c), GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
        if (size == GDI_ERROR) {  // not an outline face
            ok = false;
            break;
        }
        Glyph& g = f.g[c];
        g.adv = gm.gmCellIncX;
        const int w = (int)gm.gmBlackBoxX, h = (int)gm.gmBlackBoxY, pitch = (w + 3) & ~3;
        if (!size || w <= 0 || h <= 0) continue;  // a space
        if (w > kMaxSide || h > kMaxSide || size > sizeof gRaw || (DWORD)(pitch * h) > size) continue;
        if (GetGlyphOutlineW(dc, (UINT)(kFirstChar + c), GGO_GRAY8_BITMAP, &gm, size, gRaw, &identity) == GDI_ERROR) continue;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int q = (std::min<int>(gRaw[y * pitch + x], 64) + 2) / 4;  // 0..16
                gCov[y * w + x] = (uint8_t)(q * 255 / 16);
            }
        if (kFirstChar + c == 'H') f.capH = h;
        ok = AddLayers(f, g, w, h, gm.gmptGlyphOrigin.x, -gm.gmptGlyphOrigin.y);
        ++drawn;
    }
    SelectObject(dc, old);
    DeleteObject(font);
    if (!ok || drawn < 60 || f.capH < 4) return false;
    return AddWifi(f);
}

Face gMain, gSmall;
int gBuiltW = 0, gBuiltH = 0, gBuilds = 0;
bool gBuilt = false, gFacesOk = false;
uint64_t gBuiltAtMs = 0;

// ---------------------------------------------------------------------------------------------------------------
// Rectangles.

struct Canvas {
    Vtx* v;
    int n, cap, dropped;
};

void Quad(Canvas& c, int x0, int y0, int x1, int y1, uint32_t argb) {
    if (x1 <= x0 || y1 <= y0 || !(argb >> 24)) return;
    if (c.n + 6 > c.cap) {
        ++c.dropped;
        return;
    }
    const float l = (float)x0 - 0.5f, t = (float)y0 - 0.5f, r = (float)x1 - 0.5f, b = (float)y1 - 0.5f;
    Vtx* v = c.v + c.n;
    v[0] = Vtx{l, t, 0, 1, argb}, v[1] = Vtx{r, t, 0, 1, argb}, v[2] = Vtx{l, b, 0, 1, argb};
    v[3] = Vtx{r, t, 0, 1, argb}, v[4] = Vtx{r, b, 0, 1, argb}, v[5] = Vtx{l, b, 0, 1, argb};
    c.n += 6;
}

// Labels in a pale gold (the game's HUD writes its names in gold): on a bright stage a grey label lost against the
// white numbers beside it.
constexpr uint32_t kWhite = 0xF4F4F4, kLabel = 0xE9D9A6, kGreen = 0x56E36F, kYellow = 0xFFCF3A, kRed = 0xFF5247,
                   kUnlit = 0x7A7F87, kBlack = 0x000000;
constexpr int kEdgeAlpha = 226;

uint32_t Argb(uint32_t rgb, int alpha) { return ((uint32_t)std::clamp(alpha, 0, 255) << 24) | (rgb & 0xFFFFFF); }

void Runs(Canvas& c, const Face& f, int first, int count, int x, int y, uint32_t rgb, int alpha) {
    const int z = f.zoom;
    for (const Run *r = f.runs + first, *end = r + count; r < end; ++r)
        Quad(c, x + r->x0 * z, y + r->y0 * z, x + r->x1 * z, y + r->y1 * z, Argb(rgb, r->a * alpha / 255));
}

const Glyph* GlyphOf(const Face& f, char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);  // the faces hold capitals only
    const int i = (unsigned char)ch - kFirstChar;
    return i >= 0 && i < kChars ? &f.g[i] : nullptr;
}

int TextW(const Face& f, const char* s) {
    int w = 0;
    for (; *s; ++s)
        if (const Glyph* g = GlyphOf(f, *s)) w += g->adv * f.zoom;
    return w;
}

// The string's dark edge first, then its glyphs: a letter's edge never covers its neighbour. Returns the width.
int Text(Canvas& c, const Face& f, const char* s, int x, int y, uint32_t rgb, int alpha) {
    int pen = x;
    for (const char* p = s; *p; ++p)
        if (const Glyph* g = GlyphOf(f, *p)) {
            Runs(c, f, g->edge, g->edgeN, pen, y, kBlack, kEdgeAlpha * alpha / 255);
            pen += g->adv * f.zoom;
        }
    pen = x;
    for (const char* p = s; *p; ++p)
        if (const Glyph* g = GlyphOf(f, *p)) {
            Runs(c, f, g->fill, g->fillN, pen, y, rgb, alpha);
            pen += g->adv * f.zoom;
        }
    return pen - x;
}

int WifiW(const Face& f) { return f.g[kWifiGlyph].adv * f.zoom; }
void Wifi(Canvas& c, const Face& f, int x, int y, uint32_t rgb, int alpha) {
    const Glyph& g = f.g[kWifiGlyph];
    Runs(c, f, g.edge, g.edgeN, x, y, kBlack, kEdgeAlpha * alpha / 255);
    Runs(c, f, g.fill, g.fillN, x, y, rgb, alpha);
}

// Three connection bars standing on the baseline, the tallest as high as a capital letter.
struct BarDims {
    int bw, gap, h[3];
};
BarDims Bars3(const Face& f) {
    BarDims d;
    d.bw = std::max(2, (int)std::lround(f.Em() * 0.17));
    d.gap = std::max(f.Reach(), (int)std::lround(f.Em() * 0.075));
    d.h[2] = f.Cap();
    d.h[1] = std::max(2, (int)std::lround(f.Cap() * 0.70));
    d.h[0] = std::max(1, (int)std::lround(f.Cap() * 0.42));
    return d;
}
int BarsW(const Face& f) {
    const BarDims d = Bars3(f);
    return 3 * d.bw + 2 * d.gap;
}
void Bars(Canvas& c, const Face& f, int x, int y, int lit, uint32_t rgb, int alpha) {
    const BarDims d = Bars3(f);
    const int e = f.Reach();
    // One dark shape behind the three (columns that meet in the gaps, never overlapping).
    for (int i = 0; i < 3; ++i) {
        const int l = x + i * (d.bw + d.gap), r = l + d.bw;
        const int dl = i == 0 ? l - e : l - d.gap / 2, dr = i == 2 ? r + e : r + (d.gap - d.gap / 2);
        Quad(c, dl, y - d.h[i] - e, dr, y + e, Argb(kBlack, kEdgeAlpha * alpha / 255));
    }
    for (int i = 0; i < 3; ++i) {
        const int l = x + i * (d.bw + d.gap);
        Quad(c, l, y - d.h[i], l + d.bw, y, Argb(i < lit ? rgb : kUnlit, (i < lit ? 255 : 150) * alpha / 255));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// What to show.

void Upper(char* s) {
    for (; *s; ++s)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

// "MK9 NETCODE  (the opponent does not run the mod)" -> "the opponent does not run the mod"; any other text as it is.
void Reason(const char* text, char* out, size_t n) {
    const char* open = strchr(text, '(');
    const char* close = open ? strrchr(open, ')') : nullptr;
    if (!strncmp(text, "MK9 NETCODE", 11) && open && close && close > open + 1)
        snprintf(out, n, "%.*s", (int)(close - open - 1), open + 1);
    else
        snprintf(out, n, "%s", text);
}

struct Note {
    char text[80];
    uint32_t rgb;
};
struct GroupLook {  // the connection group under the timer
    int lit = 0;
    uint32_t barsRgb = kGreen, depthRgb = kWhite;
    char ping[8] = "", depth[8] = "", delay[8] = "";
    bool depthOn = false;
    bool wifiInside = false;  // a Wi-Fi mark in the group: a side is on Wi-Fi and the sides are not known
};
struct TagLook {  // in the group's place while no session runs
    char text[32] = "";
    uint32_t rgb = kLabel;
};
struct NotesLook {
    int n = 0;
    Note note[2];
};
struct PreLook {  // the badge before a fight
    char head[40] = "", ping[8] = "", tail[80] = "";
    int lit = 0;  // 0: no bars and no ping
    uint32_t barsRgb = kGreen, headRgb = kWhite;
};
struct Wanted {
    bool group = false, tag = false, pre = false;
    GroupLook g;
    TagLook t;
    NotesLook notes;
    PreLook p;
    int wifiSides = 0;  // bit 0: a mark at player 1's side, bit 1: at player 2's
};

uint32_t QualityRgb(int q) { return q == 0 ? kGreen : q == 1 ? kYellow : kRed; }

void AddNote(Wanted& w, uint32_t rgb, const char* text) {
    if (w.notes.n >= 2) return;
    Note& n = w.notes.note[w.notes.n++];
    snprintf(n.text, sizeof n.text, "%s", text);
    Upper(n.text);
    n.rgb = rgb;
}

// Render thread only: when the current "MK9 netcode" reason first showed, and the last stop.
char gInfoText[64] = "";
uint64_t gInfoSince = 0, gStoppedAt = 0;
bool gStoppedSeen = false;

void Compose(const Stats& s, uint64_t now, Wanted& w) {
    char buf[96];
    const bool live = s.mode == Overlay::kRollback || s.mode == Overlay::kLockstep;
    const bool before = s.phase == Overlay::kPhaseSelect || s.phase == Overlay::kPhaseLoad;
    const bool infoInFight = s.mode == Overlay::kInfo && !before;
    if (!infoInFight) gInfoText[0] = 0;
    if (s.hudHidden) return;  // the game's HUD is away: so is this one
    if (live) {
        w.group = true;
        const int q = Hud::Quality(s);
        w.g.lit = 3 - q;
        w.g.barsRgb = QualityRgb(q);
        const int rtt = s.rttMs >= 0 ? s.rttMs : s.ping;
        if (rtt >= 0) snprintf(w.g.ping, sizeof w.g.ping, "%d", std::min(rtt, 999));
        else snprintf(w.g.ping, sizeof w.g.ping, "--");
        w.g.depthOn = s.mode == Overlay::kRollback;
        snprintf(w.g.depth, sizeof w.g.depth, "%u", (unsigned)std::min<int>(s.maxDepth, 99));
        w.g.depthRgb = s.maxDepth >= 5 ? kRed : s.maxDepth >= 3 ? kYellow : kWhite;
        snprintf(w.g.delay, sizeof w.g.delay, "%d", std::clamp<int>(s.delay, 0, 99));
        const int mine = s.wifi & 1, theirs = (s.wifi >> 1) & 1;
        if (s.localSide == 1) w.wifiSides = mine | theirs << 1;
        else if (s.localSide == 2) w.wifiSides = theirs | mine << 1;
        else w.g.wifiInside = mine || theirs;
        if (s.mismatched > 0) {
            snprintf(buf, sizeof buf, "Desync: %d frames differ", (int)s.mismatched);
            AddNote(w, kRed, buf);
        }
        if (s.peerInterrupted) {
            AddNote(w, kRed, "Opponent not responding");
        } else {
            const bool stall = s.netStallMs >= 150 && s.netStallAgoS <= 4;
            const bool hitch = s.localHitchMs >= 100 && s.localHitchAgoS <= 4;
            if (hitch && (!stall || s.localHitchMs >= s.netStallMs)) {
                snprintf(buf, sizeof buf, "This PC hitched: %u ms", (unsigned)s.localHitchMs);
                AddNote(w, kYellow, buf);
            } else if (stall) {
                snprintf(buf, sizeof buf, "Connection hiccup: %.1f s", s.netStallMs / 1000.0);
                AddNote(w, kYellow, buf);
            }
        }
        return;
    }
    switch (s.mode) {
    case Overlay::kStopped:
        gStoppedAt = now;
        gStoppedSeen = true;
        snprintf(buf, sizeof buf, "Rollback stopped: %s", Hud::StopText(s.stopWhy));
        AddNote(w, kRed, buf);
        AddNote(w, kLabel, "MK9 netcode plays the rest of this match");
        return;
    case Overlay::kSyncTest:
        w.tag = true;
        snprintf(w.t.text, sizeof w.t.text, "SYNCTEST  %d / %d", (int)s.loads, (int)s.replayed);
        return;
    case Overlay::kWaiting:
        if (before && s.pre) break;  // the badge says it
        w.tag = true;
        snprintf(w.t.text, sizeof w.t.text, "ROLLBACK STARTING");
        return;
    case Overlay::kInfo:
        if (before) break;
        // An online fight on MK9's own netcode: a quiet tag for the whole fight, the reason for its first seconds.
        w.tag = true;
        snprintf(w.t.text, sizeof w.t.text, "MK9 NETCODE");
        if (strncmp(gInfoText, s.stopWhy, sizeof gInfoText - 1)) {
            snprintf(gInfoText, sizeof gInfoText, "%s", s.stopWhy);
            gInfoSince = now;
        }
        if (now - gInfoSince < 6000 && !(gStoppedSeen && now - gStoppedAt < 20000)) {  // not again right after a stop's own note
            Reason(s.stopWhy, buf, sizeof buf);
            AddNote(w, kLabel, buf);
        }
        return;
    default:
        break;
    }
    if (!before) return;
    if (s.pre == Overlay::kPreReady) {
        w.pre = true;
        snprintf(w.p.head, sizeof w.p.head, "ROLLBACK READY");
        if (s.preRttMs >= 0) {
            const int q = s.preRttMs > 120 ? 2 : s.preRttMs > 60 ? 1 : 0;
            w.p.lit = 3 - q;
            w.p.barsRgb = QualityRgb(q);
            snprintf(w.p.ping, sizeof w.p.ping, "%d", std::min<int>(s.preRttMs, 999));
        }
    } else if (s.pre == Overlay::kPreNoMod) {
        w.pre = true;
        w.p.headRgb = kLabel;
        snprintf(w.p.head, sizeof w.p.head, "MK9 NETCODE");
        snprintf(w.p.tail, sizeof w.p.tail, "OPPONENT HAS NO MOD");
    } else if (s.pre == Overlay::kPreBlocked) {
        w.pre = true;
        w.p.headRgb = kLabel;
        snprintf(w.p.head, sizeof w.p.head, "MK9 NETCODE");
        Reason(s.stopWhy, w.p.tail, sizeof w.p.tail);
        Upper(w.p.tail);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Where. Positions are in 1/1080 of the height of a 16:9 frame centred in the picture (MK9 letterboxes to 16:9),
// calibrated on screenshots of the fight HUD.

struct Frame {
    int x, y, w, h;
    float u;
};
Frame FrameOf(const Hud::View& v) {
    Frame f;
    if ((long long)v.w * 9 >= (long long)v.h * 16) {
        f.h = v.h;
        f.w = (int)((long long)v.h * 16 / 9);
    } else {
        f.w = v.w;
        f.h = (int)((long long)v.w * 9 / 16);
    }
    f.x = v.x + (v.w - f.w) / 2;
    f.y = v.y + (v.h - f.h) / 2;
    f.u = (float)f.h / 1080.0f;
    return f;
}

// Measured 2026-11-12 on frames of a Ladder fight and of its character select at 1920x1200 (a 1920x1080 frame, 'hudshot'):
//   the round timer's medallion spans 30..130 below the frame's top, and its middle spike points down to 157;
//   a round-win mark (a skull) sits at 67..90 beside the middle, 105..134 down; the health bars end at 95;
//   a portrait reaches 190 in from the frame's side, the fighter's name starts at 219.
constexpr float kMainEm = 30.0f, kSmallEm = 21.0f;   // the two faces' em heights
constexpr float kGroupBase = 162.0f;                 // baseline of the group under the round timer: under the round-win marks
constexpr float kSpikeGap = 10.0f;                   // the group's two halves keep this far from the middle: the spike's room
constexpr float kWifiX = 214.0f, kWifiBase = 130.0f; // the Wi-Fi mark at a player's side: under the bar, beside the portrait
constexpr float kPreBase = 66.0f;                    // baseline of the badge before a fight: in the roster's top medallion

int U(const Frame& f, float units) { return (int)std::lround(units * f.u); }

// The group in two halves beside the timer's spike: the connection (bars, round trip) on its left, the frames rolled
// back and the input delay on its right. Whatever the numbers' widths, nothing is drawn across the spike.
void DrawGroup(Canvas& c, const Frame& fr, const GroupLook& g, int alpha) {
    const Face &M = gMain, &S = gSmall;
    const int em = M.Em();
    const int tight = std::max(M.Reach() + 1, (int)std::lround(em * 0.10)), mid = (int)std::lround(em * 0.34);
    const int cx = fr.x + fr.w / 2, gap = U(fr, kSpikeGap);
    const int y = fr.y + U(fr, kGroupBase);
    int left = BarsW(M) + mid + TextW(M, g.ping) + tight + TextW(S, "MS");
    if (g.wifiInside) left += WifiW(M) + mid;
    int x = cx - gap - left;
    if (g.wifiInside) {
        Wifi(c, M, x, y, kYellow, alpha);
        x += WifiW(M) + mid;
    }
    Bars(c, M, x, y, g.lit, g.barsRgb, alpha);
    x += BarsW(M) + mid;
    x += Text(c, M, g.ping, x, y, kWhite, alpha) + tight;
    Text(c, S, "MS", x, y, kLabel, alpha);
    x = cx + gap;
    if (g.depthOn) {
        x += Text(c, S, "R", x, y, kLabel, alpha) + tight;
        x += Text(c, M, g.depth, x, y, g.depthRgb, alpha) + mid;
    }
    x += Text(c, S, "D", x, y, kLabel, alpha) + tight;
    Text(c, M, g.delay, x, y, kWhite, alpha);
}

void DrawCentred(Canvas& c, const Frame& fr, const Face& f, const char* text, int y, uint32_t rgb, int alpha) {
    Text(c, f, text, fr.x + fr.w / 2 - TextW(f, text) / 2, y, rgb, alpha);
}

// A tag in the group's row: its first word ends left of the spike, the rest starts right of it ("MK9 | NETCODE").
void DrawSplit(Canvas& c, const Frame& fr, const Face& f, const char* text, int y, uint32_t rgb, int alpha) {
    const char* space = strchr(text, ' ');
    if (!space) return DrawCentred(c, fr, f, text, y, rgb, alpha);
    char first[40];
    snprintf(first, sizeof first, "%.*s", (int)(space - text), text);
    while (*space == ' ') ++space;
    const int cx = fr.x + fr.w / 2, gap = U(fr, kSpikeGap);
    Text(c, f, first, cx - gap - TextW(f, first), y, rgb, alpha);
    Text(c, f, space, cx + gap, y, rgb, alpha);
}

void DrawNotes(Canvas& c, const Frame& fr, const NotesLook& n, int alpha) {
    const int step = (int)std::lround(gSmall.Em() * 1.18);
    int y = fr.y + U(fr, kGroupBase) + step + gSmall.Reach();
    for (int i = 0; i < n.n; ++i, y += step) DrawCentred(c, fr, gSmall, n.note[i].text, y, n.note[i].rgb, alpha);
}

void DrawPre(Canvas& c, const Frame& fr, const PreLook& p, int alpha) {
    const Face &M = gMain, &S = gSmall;
    const int em = M.Em();
    const int tight = std::max(M.Reach() + 1, (int)std::lround(em * 0.10)), mid = (int)std::lround(em * 0.34),
              wide = (int)std::lround(em * 0.62);
    // Two lines start higher: the second one stays above the gold arc over the roster (96 below the frame's top).
    const int y = fr.y + U(fr, p.tail[0] ? kPreBase - 14.0f : kPreBase);
    if (p.lit) {  // ROLLBACK READY  [bars] 33 MS
        const int total = TextW(M, p.head) + wide + BarsW(M) + mid + TextW(M, p.ping) + tight + TextW(S, "MS");
        int x = fr.x + fr.w / 2 - total / 2;
        x += Text(c, M, p.head, x, y, p.headRgb, alpha) + wide;
        Bars(c, M, x, y, p.lit, p.barsRgb, alpha);
        x += BarsW(M) + mid;
        x += Text(c, M, p.ping, x, y, kWhite, alpha) + tight;
        Text(c, S, "MS", x, y, kLabel, alpha);
        return;
    }
    DrawCentred(c, fr, M, p.head, y, p.headRgb, alpha);
    if (p.tail[0]) DrawCentred(c, fr, S, p.tail, y + (int)std::lround(S.Em() * 1.18) + S.Reach(), kLabel, alpha);
}

// Fades (render thread only): each part keeps what it last showed while it fades out.
struct Shown {
    float group = 0, tag = 0, notes = 0, pre = 0, wifi[2] = {0, 0};
    GroupLook g;
    TagLook t;
    NotesLook n;
    PreLook p;
    uint64_t last = 0;
};
Shown gShown;
int gDropped = 0;

float Step(float a, bool on, float dtMs) {
    if (dtMs > 400.0f) return on ? 1.0f : 0.0f;  // the first frame, or a display that was away: no slow fade from stale state
    return on ? std::min(1.0f, a + dtMs / 160.0f) : std::max(0.0f, a - dtMs / 240.0f);
}
int A8(float a) { return (int)(a * 255.0f + 0.5f); }

}  // namespace

namespace Hud {

int Quality(const Stats& s) {
    const int rtt = s.rttMs >= 0 ? s.rttMs : s.ping;  // GGPO's ping is quantized to frames: only a fallback
    const bool stallRecent = s.netStallAgoS <= 5;
    if ((rtt > 120) || s.maxDepth >= 5 || s.peerInterrupted || (stallRecent && s.netStallMs >= 500)) return 2;
    if ((rtt > 60) || s.maxDepth >= 3 || s.loadsPerSec > 20 || (stallRecent && s.netStallMs >= 150)) return 1;
    return 0;
}

const char* StopText(const char* why) {
    struct Map {
        const char* key;
        const char* text;
    };
    static const Map kMap[] = {
        {"peer disconnected", "Opponent disconnected"},
        {"did not join", "Opponent's game did not start the session"},
        {"not starting the session", "Opponent's game did not start the session"},
        {"environment mismatch", "Different mod version or game files"},
        {"frame 0 differs", "Games started out of sync"},
        {"different states", "Games started out of sync"},
        {"Safe/Rollback", "Different Safe/Rollback setting"},
        {"script VM was busy", "Internal timing problem"},
    };
    for (const Map& m : kMap)
        if (strstr(why, m.key)) return m.text;
    return why;
}

bool Prepare(const View& v, bool* wait) {
    if (wait) *wait = false;
    if (gBuilt && v.w == gBuiltW && v.h == gBuiltH) return gFacesOk;
    // A build takes 10-20 ms of the rendering thread. A picture size that changed again within two seconds waits, and
    // the display draws nothing meanwhile: a game that flipped between two viewports would otherwise rebuild on every
    // frame.
    const uint64_t now = GetTickCount64();
    if (gBuilt && now - gBuiltAtMs < 2000) {
        if (wait) *wait = true;
        return false;
    }
    gBuiltAtMs = now;
    ++gBuilds;
    gBuilt = true;
    gBuiltW = v.w;
    gBuiltH = v.h;
    gFacesOk = false;
    if (v.w < 320 || v.h < 200) return false;
    const Frame fr = FrameOf(v);
    const int zoom = fr.h > 2880 ? 3 : fr.h > 1440 ? 2 : 1;
    // Not below 16 and 12 pixels: a 540-line window (the test harness) keeps readable labels.
    const int mainPx = std::max(16, (int)std::lround(kMainEm * fr.u / zoom));
    const int smallPx = std::max(12, (int)std::lround(kSmallEm * fr.u / zoom));
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    gFacesOk = BuildFace(gMain, dc, mainPx, zoom) && BuildFace(gSmall, dc, smallPx, zoom);
    DeleteDC(dc);
    return gFacesOk;
}

int Build(const Stats& s, const View& v, uint64_t nowMs, Vtx* out, int cap) {
    if (!gFacesOk || v.w != gBuiltW || v.h != gBuiltH) return 0;
    Wanted w;
    Compose(s, nowMs, w);
    Shown& sh = gShown;
    const float dt = sh.last ? (float)(nowMs - sh.last) : 1e9f;
    sh.last = nowMs;
    if (w.group) sh.g = w.g;
    if (w.tag) sh.t = w.t;
    if (w.notes.n) sh.n = w.notes;
    if (w.pre) sh.p = w.p;
    sh.group = Step(sh.group, w.group, dt);
    sh.tag = Step(sh.tag, w.tag, dt);
    sh.notes = Step(sh.notes, w.notes.n > 0, dt);
    sh.pre = Step(sh.pre, w.pre, dt);
    for (int i = 0; i < 2; ++i) sh.wifi[i] = Step(sh.wifi[i], (w.wifiSides >> i) & 1, dt);
    Canvas c{out, 0, cap, 0};
    const Frame fr = FrameOf(v);
    if (sh.group > 0) DrawGroup(c, fr, sh.g, A8(sh.group));
    if (sh.tag > 0) DrawSplit(c, fr, gSmall, sh.t.text, fr.y + U(fr, kGroupBase), sh.t.rgb, A8(sh.tag));
    if (sh.notes > 0) DrawNotes(c, fr, sh.n, A8(sh.notes));
    if (sh.pre > 0) DrawPre(c, fr, sh.p, A8(sh.pre));
    for (int i = 0; i < 2; ++i) {
        if (sh.wifi[i] <= 0) continue;
        const int x = i == 0 ? fr.x + U(fr, kWifiX) : fr.x + fr.w - U(fr, kWifiX) - WifiW(gMain);
        Wifi(c, gMain, x, fr.y + U(fr, kWifiBase), kYellow, A8(sh.wifi[i]));
    }
    gDropped = c.dropped;
    return c.n;
}

int Dropped() { return gDropped; }
int Builds() { return gBuilds; }

void Describe(char* out, int n) {
    char face[LF_FACESIZE * 3] = "";
    WideCharToMultiByte(CP_UTF8, 0, gFaceName, -1, face, sizeof face, nullptr, nullptr);
    snprintf(out, n, "%s for %dx%d: face \"%s\", %d and %d px (x%d), %d and %d rectangles", gFacesOk ? "built" : "NOT built", gBuiltW,
             gBuiltH, face, gMain.px, gSmall.px, gMain.zoom, gMain.n, gSmall.n);
}

bool Demo(int n, Stats* s) {
    *s = Stats{};
    s->phase = Overlay::kPhaseFight;
    s->localSide = 1;
    switch (n) {
    case 1:  // a good connection
        s->mode = Overlay::kRollback, s->rttMs = 33, s->ping = 33, s->delay = 2, s->maxDepth = 1, s->lastDepth = 1;
        return true;
    case 2:  // a so-so one: both on Wi-Fi, a hiccup a moment ago
        s->mode = Overlay::kRollback, s->rttMs = 87, s->delay = 3, s->maxDepth = 3, s->wifi = 3;
        s->netStallMs = 400, s->netStallAgoS = 1;
        return true;
    case 3:  // a bad one: the opponent on Wi-Fi and not responding
        s->mode = Overlay::kRollback, s->rttMs = 164, s->delay = 4, s->maxDepth = 6, s->wifi = 2, s->peerInterrupted = 1;
        return true;
    case 4:
        s->mode = Overlay::kStopped;
        snprintf(s->stopWhy, sizeof s->stopWhy, "peer disconnected");
        return true;
    case 5:
        s->mode = Overlay::kInfo;
        snprintf(s->stopWhy, sizeof s->stopWhy, "MK9 NETCODE  (the opponent does not run the mod)");
        return true;
    case 6:
        s->phase = Overlay::kPhaseSelect, s->mode = Overlay::kInfo, s->pre = Overlay::kPreReady, s->preRttMs = 33;
        return true;
    case 7:
        s->phase = Overlay::kPhaseSelect, s->mode = Overlay::kInfo, s->pre = Overlay::kPreBlocked;
        snprintf(s->stopWhy, sizeof s->stopWhy, "MK9 NETCODE  (the other player's mod is older)");
        return true;
    case 8:  // Safe mode: no rollbacks to count
        s->mode = Overlay::kLockstep, s->rttMs = 48, s->delay = 2;
        return true;
    case 9:
        s->mode = Overlay::kRollback, s->rttMs = 41, s->delay = 2, s->maxDepth = 2, s->mismatched = 12;
        return true;
    case 10:
        s->mode = Overlay::kWaiting;
        return true;
    case 11:
        s->phase = Overlay::kPhaseLoad, s->pre = Overlay::kPreNoMod;
        return true;
    case 12:  // the most rectangles: widest numbers, both marks, two long notes
        s->mode = Overlay::kRollback, s->rttMs = 888, s->delay = 8, s->maxDepth = 88, s->wifi = 3, s->mismatched = 88888;
        s->localHitchMs = 8888, s->localHitchAgoS = 0;
        return true;
    default:
        return false;
    }
}

void Reset() {
    gShown = Shown{};
    gInfoText[0] = 0;
    gInfoSince = gStoppedAt = 0;
    gStoppedSeen = false;
    gBuilt = gFacesOk = false;
    gBuiltW = gBuiltH = 0;
    gBuiltAtMs = 0;
}

}  // namespace Hud
