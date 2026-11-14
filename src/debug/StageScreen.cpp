#include "StageScreen.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ArenaPics.h"
#include "StageSkin.h"

namespace {
using Hud::Canvas;
using Hud::FaceId;
using Hud::Frame;
using Hud::kMainFace;
using Hud::kSmallFace;
using namespace StageScreen;

constexpr uint32_t kWhite = 0xF4F4F4, kLabel = 0xE9D9A6, kGreen = 0x56E36F, kOrange = 0xFFA348, kYellow = 0xFFCF3A, kRed = 0xFF5247,
                   kGold = 0xE7B93C, kInk = 0x0C0C10, kGround = 0x0A0A0E;
constexpr uint32_t kBarMine = 0x1F6B36, kBarTheirs = 0x8A4B12, kBarCursor = 0x4B4738;

uint32_t Argb(uint32_t rgb, int alpha) { return ((uint32_t)std::clamp(alpha, 0, 255) << 24) | (rgb & 0xFFFFFF); }
int U(const Frame& f, float units) { return (int)std::lround(units * f.u); }

// Positions in 1/1080 of the frame's height, like the HUD display's.
struct Dims {
    int cols;
    float cellW, rowH;
    FaceId face;
};
Dims DimsOf(int cells) { return cells <= 30 ? Dims{3, 440.0f, 46.0f, kMainFace} : Dims{4, 335.0f, 38.0f, kSmallFace}; }
// The panel's own lines: half its width, top, bottom, the grid's top, the baselines of the title, the line under it
// and the three lines below the grid.
struct Geo {
    float halfW, top, bottom, gridTop, titleBase, subBase, lineA, lineB, lineC;
};
constexpr Geo kListGeo{700.0f, 110.0f, 892.0f, 242.0f, 172.0f, 208.0f, 772.0f, 815.0f, 852.0f};
// The tiles' panel takes most of the frame: 8 tiles of 192 by 158 (a picture 176 by 99 and two lines of its name).
constexpr Geo kTileGeo{800.0f, 56.0f, 990.0f, 178.0f, 112.0f, 146.0f, 866.0f, 908.0f, 946.0f};
constexpr float kTileW = 192.0f, kTileH = 158.0f, kTilePicInset = 8.0f;
constexpr float kTileNameOne = 140.0f, kTileNameA = 129.0f, kTileNameB = 152.0f;  // the name's baselines below the tile's top
constexpr uint32_t kTileGround = 0x16161C;
constexpr float kBannerA = 980.0f, kBannerB = 1016.0f;

void Centre(Canvas& c, const Frame& fr, FaceId f, const char* t, float base, uint32_t rgb, bool edge) {
    Hud::Write(c, f, t, fr.x + fr.w / 2 - Hud::Width(f, t) / 2, fr.y + U(fr, base), rgb, 255, edge);
}
void Outline(Canvas& c, const Box& b, int t, uint32_t argb) {
    Hud::Rect(c, b.x0, b.y0, b.x1, b.y0 + t, argb);
    Hud::Rect(c, b.x0, b.y1 - t, b.x1, b.y1, argb);
    Hud::Rect(c, b.x0, b.y0 + t, b.x0 + t, b.y1 - t, argb);
    Hud::Rect(c, b.x1 - t, b.y0 + t, b.x1, b.y1 - t, argb);
}
// The text cut to a width (a name longer than its cell loses its end, never its neighbours' room). 'width' measures
// a text in the face it is written with.
template <typename W>
const char* FitBy(W width, const char* text, int maxW, char* buf, size_t n) {
    snprintf(buf, n, "%s", text);
    size_t len = strlen(buf);
    while (len && width(buf) > maxW) buf[--len] = 0;
    return buf;
}
const char* Fit(FaceId f, const char* text, int maxW, char* buf, size_t n) {
    return FitBy([f](const char* t) { return Hud::Width(f, t); }, text, maxW, buf, n);
}
template <typename W>
int WrapBy(W width, const char* text, int maxW, char* a, char* b, int cap) {
    if (cap <= 0) return 1;
    a[0] = b[0] = 0;
    if (!text) text = "";
    snprintf(a, (size_t)cap, "%s", text);
    if (width(a) <= maxW) return 1;
    char left[64], right[64];
    int best = -1, bestW = INT_MAX;
    for (int i = 0; text[i] && i < 60; ++i) {
        if (text[i] != ' ') continue;
        snprintf(left, sizeof left, "%.*s", i, text);
        snprintf(right, sizeof right, "%s", text + i + 1);
        const int w = std::max(width(left), width(right));
        if (w < bestW) bestW = w, best = i;
    }
    if (best < 0) {  // one word: cut
        FitBy(width, text, maxW, a, (size_t)cap);
        return 1;
    }
    snprintf(left, sizeof left, "%.*s", best, text);
    snprintf(right, sizeof right, "%s", text + best + 1);
    FitBy(width, left, maxW, a, (size_t)cap);
    FitBy(width, right, maxW, b, (size_t)cap);
    return 2;
}
const char* WinnerText(uint8_t w) {
    return w == kMine ? "YOUR PICK" : w == kTheirs ? "OPPONENT'S PICK" : w == kBoth ? "BOTH PLAYERS PICKED IT" : "";
}
uint32_t WinnerRgb(uint8_t w) { return w == kMine ? kGreen : w == kTheirs ? kOrange : kWhite; }
const char* NameOf(const State& s, uint8_t cell) { return cell < s.cells ? s.name[cell] : "?"; }

// Peer- and thread-proof copy: every index inside the cells, every text terminated.
void Sanitize(State& s) {
    if (s.cells > kMaxCells) s.cells = kMaxCells;
    for (uint8_t* p : {&s.randomCell, &s.myCursor, &s.theirCursor, &s.resultCell})
        if (*p >= s.cells) *p = kNoCell;
    for (int i = 0; i < kMaxCells; ++i) s.name[i][sizeof s.name[i] - 1] = 0;
    for (uint8_t& p : s.pic)
        if (p > ArenaPics::kSlots) p = 0;
    s.note[sizeof s.note - 1] = 0;
    if (s.phase > kCard) s.phase = kOff;
    if (s.winner > kBoth) s.winner = kNoWinner;
}

// The pictures of the frame being built (the rendering thread; the host test is one thread).
Pic gPics[kMaxCells];
int gPicN = 0, gSplit = 0;

// The grid as picture tiles. Under the pictures: each tile's card (the colour of a pick, of the cursor, of the draw)
// and the picture's dark ground. Over them: the shade of a picture that is not in the draw, the cursors' outlines,
// the strip that says whose pick it is, the name.
void DrawTiles(Canvas& c, const State& s, const Layout& l, const Frame& fr, uint8_t lit, bool myLock, bool theirLock) {
    const bool picking = s.phase == kPicking, roulette = s.phase == kRoulette, result = s.phase == kResult;
    const int inset = std::max(1, U(fr, 3.0f)), line = std::max(1, U(fr, 3.0f)), namePad = U(fr, 4.0f);
    struct Cell {
        bool mine, theirs, mineL, theirsL, gold;
    };
    const auto cellOf = [&](int i) {
        Cell k;
        k.mine = i == s.myCursor, k.theirs = i == s.theirCursor;
        k.mineL = k.mine && myLock, k.theirsL = k.theirs && theirLock;
        k.gold = (result && i == s.resultCell) || (roulette && i == lit);
        return k;
    };
    for (int i = 0; i < s.cells; ++i) {
        const Cell k = cellOf(i);
        const Box b{l.cell[i].x0 + inset, l.cell[i].y0 + inset, l.cell[i].x1 - inset, l.cell[i].y1 - inset};
        const int a = picking ? 255 : 150;
        if (k.gold) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(kGold, 255));
        } else if (k.mineL && k.theirsL) {
            const int mid = (b.x0 + b.x1) / 2;
            Hud::Rect(c, b.x0, b.y0, mid, b.y1, Argb(kBarMine, a));
            Hud::Rect(c, mid, b.y0, b.x1, b.y1, Argb(kBarTheirs, a));
        } else if (k.mineL || k.theirsL) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(k.mineL ? kBarMine : kBarTheirs, a));
        } else if (k.mine && picking) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(kBarCursor, 255));
        }
        const Box& p = l.pic[i];
        Hud::Rect(c, p.x0, p.y0, p.x1, p.y1, Argb(kTileGround, 255));
        if (s.pic[i] && gPicN < kMaxCells) gPics[gPicN++] = Pic{p.x0, p.y0, p.x1, p.y1, (uint8_t)(s.pic[i] - 1)};
    }
    gSplit = c.n;
    const int stripH = Hud::CapOf(kSmallFace) + 2 * std::max(1, U(fr, 5.0f));
    for (int i = 0; i < s.cells; ++i) {
        const Cell k = cellOf(i);
        const Box& p = l.pic[i];
        uint32_t rgb = kLabel;
        int alpha = picking ? 255 : 120, shade = picking ? 0 : 165;  // after the picks, the tiles that are not in the draw step back
        if (k.gold) rgb = kInk, alpha = 255, shade = 0;
        else if (k.mineL || k.theirsL) rgb = kWhite, alpha = picking ? 255 : 200, shade = picking ? 0 : 90;
        else if (k.mine && picking) rgb = kWhite;
        if (!s.pic[i] && i == s.randomCell)  // no picture for "random": a question mark in its place
            Hud::Write(c, kMainFace, "?", (p.x0 + p.x1) / 2 - Hud::Width(kMainFace, "?") / 2, (p.y0 + p.y1) / 2 + Hud::CapOf(kMainFace) / 2, kLabel, 255, false);
        if (shade) Hud::Rect(c, p.x0, p.y0, p.x1, p.y1, Argb(0x000000, shade));
        if (picking && k.mine && !s.myLocked) {  // this player's cursor: a white frame around the picture
            const int ring = std::max(1, p.x0 - l.cell[i].x0 - inset);
            Outline(c, Box{p.x0 - ring, p.y0 - ring, p.x1 + ring, p.y1 + ring}, ring, Argb(kWhite, 255));
        }
        if (picking && k.theirs && !k.theirsL)  // the other player's cursor, live; faint where it last stood once its packets stop
            Outline(c, p, line, Argb(kOrange, s.peer == kPeerQuiet ? 90 : 255));
        if (k.mineL || k.theirsL) {  // whose pick: a strip over the picture's foot
            const int y0 = p.y1 - stripH, mid = (p.x0 + p.x1) / 2;
            const char* tag = k.mineL && k.theirsL ? "BOTH" : k.mineL ? "YOU" : "OPP";
            Hud::Rect(c, p.x0, y0, k.mineL && k.theirsL ? mid : p.x1, p.y1, Argb(k.mineL ? kBarMine : kBarTheirs, 255));
            if (k.mineL && k.theirsL) Hud::Rect(c, mid, y0, p.x1, p.y1, Argb(kBarTheirs, 255));
            Hud::Write(c, kSmallFace, tag, mid - Hud::Width(kSmallFace, tag) / 2, p.y1 - (stripH - Hud::CapOf(kSmallFace)) / 2, kWhite, 255, false);
        }
        char a[40], b[40];
        const int maxW = l.cell[i].x1 - l.cell[i].x0 - 2 * (inset + namePad), cx = (l.cell[i].x0 + l.cell[i].x1) / 2;
        if (Wrap(kSmallFace, s.name[i], maxW, a, b, sizeof a) == 1) {
            Hud::Write(c, kSmallFace, a, cx - Hud::Width(kSmallFace, a) / 2, l.cell[i].y0 + U(fr, kTileNameOne), rgb, alpha, false);
        } else {
            Hud::Write(c, kSmallFace, a, cx - Hud::Width(kSmallFace, a) / 2, l.cell[i].y0 + U(fr, kTileNameA), rgb, alpha, false);
            Hud::Write(c, kSmallFace, b, cx - Hud::Width(kSmallFace, b) / 2, l.cell[i].y0 + U(fr, kTileNameB), rgb, alpha, false);
        }
    }
}

// ---- MK9's look (StageSkin.h): the picture tiles drawn with the game's own lettering and frame pieces ----
namespace Sk = StageSkin;
constexpr int kLookMaxV = 30000;  // 5000 rectangles; the largest frame of the host test is a fifth of it
Sk::Vtx gLookV[kLookMaxV];
int gLookN = 0;

constexpr uint32_t kGoldLight = 0xF6E2A6, kGoldMid = 0xC9A24E, kGoldDark = 0x6E5220, kBlood = 0xE2331F, kBloodDark = 0x8E1D12, kEmber = 0xFF9A3C;
constexpr float kLookNamePad = 6.0f;
constexpr float kLookTitle = 112.0f, kLookBarTop = 100.0f, kLookBarH = 42.0f, kLookBarInset = 150.0f, kLookSub = 163.0f;
constexpr float kLookNameOne = 142.0f, kLookNameA = 131.0f, kLookNameB = 153.0f;
constexpr float kLookSunTop = 813.0f, kLookSunH = 42.0f, kLookLineA = 893.0f, kLookLineB = 928.0f, kLookLineC = 959.0f;
constexpr float kLookRing = 84.0f, kLookRingTop = 60.0f, kLookRingRight = 34.0f;

int NameWidth(const char* t) { return Sk::Width(Sk::kName, t); }
int LookRoom(const Layout& l, const Frame& fr) { return l.cell[0].x1 - l.cell[0].x0 - 2 * U(fr, kLookNamePad); }
uint32_t Grey(int level, int alpha = 255) { return Sk::Pm((uint32_t)(level << 16 | level << 8 | level), alpha); }
void LookCentre(Sk::Canvas& c, const Frame& fr, Sk::Face f, const char* t, float base, uint32_t colour) {
    Sk::Write(c, f, t, fr.x + fr.w / 2 - Sk::Width(f, t) / 2, fr.y + U(fr, base), colour);
}
// A frame around a box, outside it: one colour along the top, another along the bottom, the sides running between.
void LookFrame(Sk::Canvas& c, const Box& b, int t, uint32_t top, uint32_t bottom) {
    Sk::Fill(c, b.x0 - t, b.y0 - t, b.x1 + t, b.y0, top);
    Sk::Fill(c, b.x0 - t, b.y1, b.x1 + t, b.y1 + t, bottom);
    Sk::FillV(c, b.x0 - t, b.y0, b.x0, b.y1, top, bottom);
    Sk::FillV(c, b.x1, b.y0, b.x1 + t, b.y1, top, bottom);
}
// A line that is brightest in its middle and gone at its ends.
void LookRule(Sk::Canvas& c, int x0, int x1, int y0, int y1, uint32_t rgb, int alpha) {
    const int mid = (x0 + x1) / 2;
    Sk::FillH(c, x0, y0, mid, y1, Sk::Pm(rgb, 0), Sk::Pm(rgb, alpha));
    Sk::FillH(c, mid, y0, x1, y1, Sk::Pm(rgb, alpha), Sk::Pm(rgb, 0));
}
// A panel's ground: nearly black with a little red, soft at its sides; MK9's red glow in its middle ('glowHalf' units
// to each side of 'cx'); a gold rule above and below.
void LookGround(Sk::Canvas& c, const Frame& fr, const Box& P, int cx, float glowHalf) {
    const int soft = U(fr, 30.0f), rule = std::max(1, U(fr, 2.0f));
    const uint32_t top = Sk::Pm(0x1D0907, 255), bottom = Sk::Pm(0x060304, 255);  // opaque: at 247 the menu's own lettering showed through
    Sk::Fill4(c, P.x0, P.y0, P.x0 + soft, P.y1, 0, top, 0, bottom);
    Sk::FillV(c, P.x0 + soft, P.y0, P.x1 - soft, P.y1, top, bottom);
    Sk::Fill4(c, P.x1 - soft, P.y0, P.x1, P.y1, top, 0, bottom, 0);
    Sk::Piece(c, ArenaPics::kRed, cx - U(fr, glowHalf), P.y0 + U(fr, 10.0f), cx + U(fr, glowHalf), P.y1 - U(fr, 10.0f), Grey(255, 165));
    LookRule(c, P.x0, P.x1, P.y0, P.y0 + rule, kGoldLight, 255);
    LookRule(c, P.x0, P.x1, P.y0 + rule, P.y0 + 2 * rule, kGoldDark, 255);
    LookRule(c, P.x0, P.x1, P.y1 - 2 * rule, P.y1 - rule, kGoldLight, 255);
    LookRule(c, P.x0, P.x1, P.y1 - rule, P.y1, kGoldDark, 255);
}
// The arena select's gold bar from x0 to x1, its top at y0: the ornaments at its ends kept in shape, the line between
// them stretched.
void LookBar(Sk::Canvas& c, const Frame& fr, int x0, int x1, int y0) {
    const int y1 = y0 + U(fr, kLookBarH), end = U(fr, kLookBarH * 72.0f / 64.0f);
    Sk::PiecePart(c, ArenaPics::kBar, 0, 72, x0, y0, x0 + end, y1, Grey(255));
    Sk::PiecePart(c, ArenaPics::kBar, 72, 440, x0 + end, y0, x1 - end, y1, Grey(255));
    Sk::PiecePart(c, ArenaPics::kBar, 440, 512, x1 - end, y0, x1, y1, Grey(255));
}
// The arena select's line with the sun, 'reach' pixels to each side of 'cx', its top at y0: the sun kept in shape.
void LookSun(Sk::Canvas& c, const Frame& fr, int cx, int y0, int reach) {
    const int y1 = y0 + U(fr, kLookSunH), half = U(fr, kLookSunH);
    Sk::PiecePart(c, ArenaPics::kSun, 0, 192, cx - reach, y0, cx - half, y1, Grey(255));
    Sk::PiecePart(c, ArenaPics::kSun, 192, 320, cx - half, y0, cx + half, y1, Grey(255));
    Sk::PiecePart(c, ArenaPics::kSun, 320, 512, cx + half, y0, cx + reach, y1, Grey(255));
}

void LookTiles(Sk::Canvas& c, const State& s, const Layout& l, const Frame& fr, uint8_t lit, bool myLock, bool theirLock) {
    const bool picking = s.phase == kPicking, roulette = s.phase == kRoulette, result = s.phase == kResult;
    const int thin = std::max(1, U(fr, 2.0f)), mid = std::max(2, U(fr, 3.0f)), thick = std::max(2, U(fr, 4.0f)), pad = std::max(1, U(fr, 5.0f));
    const int cap = Sk::CapOf(Sk::kName), maxW = LookRoom(l, fr);
    struct Cell {
        bool mine, theirs, mineL, theirsL, gold, cursor;
    };
    const auto cellOf = [&](int i) {
        Cell k;
        k.mine = i == s.myCursor, k.theirs = i == s.theirCursor;
        k.mineL = k.mine && myLock, k.theirsL = k.theirs && theirLock;
        k.gold = (result && i == s.resultCell) || (roulette && i == lit);
        k.cursor = picking && k.mine && !s.myLocked;
        return k;
    };
    // Under everything: each picture's black edge, then the light behind this player's cursor and behind the draw,
    // which reaches past its tile and shows in the gaps to its neighbours.
    for (int i = 0; i < s.cells; ++i) {
        const Box& p = l.pic[i];
        Sk::Fill(c, p.x0 - thick - 1, p.y0 - thick - 1, p.x1 + thick + 1, p.y1 + thick + 1, Sk::Pm(0x000000, 215));
    }
    for (int i = 0; i < s.cells; ++i) {
        const Cell k = cellOf(i);
        if (!k.gold && !k.cursor) continue;
        const Box& p = l.pic[i];
        Sk::Glow(c, ArenaPics::kHalo, p.x0, p.y0, p.x1, p.y1, std::max(3, U(fr, k.gold ? 16.0f : 13.0f)), Sk::Lit(k.gold ? kGoldLight : kEmber, k.gold ? 235 : 200));
    }
    for (int i = 0; i < s.cells; ++i) {
        const Cell k = cellOf(i);
        const bool mineL = k.mineL, theirsL = k.theirsL, gold = k.gold, cursor = k.cursor, theirs = k.theirs;
        const Box& p = l.pic[i];
        const Box& cell = l.cell[i];
        // How bright the tile is: full under this player's cursor, for a pick and for the draw; once the picks are in,
        // every tile that is not in the draw steps far back.
        const int level = gold || cursor || (picking && (mineL || theirsL)) ? 255 : picking ? 205 : (mineL || theirsL) ? 150 : 70;
        if (s.pic[i]) {
            Sk::Picture(c, s.pic[i] - 1, p.x0, p.y0, p.x1, p.y1, Grey(level));
        } else {  // "random" has no picture: a dark red plate and the title face's question mark
            Sk::FillV(c, p.x0, p.y0, p.x1, p.y1, Sk::Pm(0x3A0D0A, 255), Sk::Pm(0x0E0303, 255));
            const int foot = mineL || theirsL ? p.y1 - cap - 2 * pad : p.y1;  // above a pick's strip
            if (i == s.randomCell) Sk::Write(c, Sk::kTitle, "?", (p.x0 + p.x1) / 2 - Sk::Width(Sk::kTitle, "?") / 2, (p.y0 + foot) / 2 + Sk::CapOf(Sk::kTitle) / 2, Grey(level));
        }
        // The frame: thin gold; this player's cursor and the draw bright and thick; a pick in its player's colour.
        if (gold) LookFrame(c, p, thick + 1, Sk::Pm(0xFFFFFF, 255), Sk::Pm(kGoldLight, 255));
        else if (mineL && theirsL) LookFrame(c, p, mid, Sk::Pm(0xFFE38A, level), Sk::Pm(kBlood, level));
        else if (mineL) LookFrame(c, p, mid, Sk::Pm(0xFFE38A, level), Sk::Pm(0xD9A93A, level));
        else if (theirsL) LookFrame(c, p, mid, Sk::Pm(0xFF5A46, level), Sk::Pm(kBloodDark, level));
        else if (cursor) LookFrame(c, p, thick, Sk::Pm(0xFFF6D8, 255), Sk::Pm(0xF0C060, 255));
        else LookFrame(c, p, thin, Sk::Pm(kGoldMid, level), Sk::Pm(kGoldDark, level));
        if (picking && theirs && !theirsL) {  // the other player's cursor, live: red, inside the picture; faint once its packets stop
            const uint32_t red = Sk::Pm(kBlood, s.peer == kPeerQuiet ? 90 : 255);
            Sk::Fill(c, p.x0, p.y0, p.x1, p.y0 + mid, red);
            Sk::Fill(c, p.x0, p.y1 - mid, p.x1, p.y1, red);
            Sk::Fill(c, p.x0, p.y0 + mid, p.x0 + mid, p.y1 - mid, red);
            Sk::Fill(c, p.x1 - mid, p.y0 + mid, p.x1, p.y1 - mid, red);
        }
        if (mineL || theirsL) {  // whose pick: a dark strip over the picture's foot, its upper edge in the player's colour
            const int y0 = p.y1 - cap - 2 * pad, half = (p.x0 + p.x1) / 2;
            const char* tag = mineL && theirsL ? "BOTH" : mineL ? "YOU" : "OPP";
            Sk::Fill(c, p.x0, y0, p.x1, p.y1, Sk::Pm(0x000000, 200));
            Sk::Fill(c, p.x0, y0, mineL && theirsL ? half : p.x1, y0 + thin, Sk::Pm(mineL ? 0xFFE38A : kBlood, 255));
            if (mineL && theirsL) Sk::Fill(c, half, y0, p.x1, y0 + thin, Sk::Pm(kBlood, 255));
            Sk::Write(c, Sk::kName, tag, half - Sk::Width(Sk::kName, tag) / 2, p.y1 - pad, Grey(gold || picking ? 255 : 200));
        }
        char a[40], b[40];
        const int cx = (cell.x0 + cell.x1) / 2, lines = WrapBy(NameWidth, s.name[i], maxW, a, b, sizeof a);
        const int baseA = cell.y0 + U(fr, lines == 1 ? kLookNameOne : kLookNameA), baseB = cell.y0 + U(fr, kLookNameB);
        if (gold || cursor) {  // the menus' glow behind the name: two halves, brightest where they meet
            const int y0 = baseA - cap - pad, y1 = (lines == 1 ? baseA : baseB) + pad, x0 = cell.x0 + U(fr, 4.0f), x1 = cell.x1 - U(fr, 4.0f);
            const uint32_t glow = Sk::Lit(0xFFFFFF, gold ? 190 : 160);
            Sk::Piece(c, ArenaPics::kGlow, x0, y0, cx, y1, glow);
            Sk::Piece(c, ArenaPics::kGlow, cx, y0, x1, y1, glow, true);
        }
        const uint32_t ink = Grey(gold || cursor ? 255 : level == 255 ? 245 : level == 205 ? 215 : level + 40);
        Sk::Write(c, Sk::kName, a, cx - Sk::Width(Sk::kName, a) / 2, baseA, ink);
        if (lines == 2) Sk::Write(c, Sk::kName, b, cx - Sk::Width(Sk::kName, b) / 2, baseB, ink);
    }
}

void DrawLook(Sk::Canvas& c, const State& s, const Hud::View& v, uint64_t now) {
    const Frame fr = Hud::FrameOf(v);
    const Layout l = LayoutOf(s.cells, true, v);
    const Box& P = l.panel;
    const int cx = fr.x + fr.w / 2;
    LookGround(c, fr, P, cx, 720.0f);

    const bool picking = s.phase == kPicking, roulette = s.phase == kRoulette;
    const bool theirLock = s.peer == kPeerLocked && s.theirCursor != kNoCell;
    const bool myLock = s.myLocked && s.myCursor != kNoCell;
    uint8_t lit = kNoCell;
    if (roulette && myLock && theirLock) lit = RouletteLit(s, now) == 0 ? s.myCursor : s.theirCursor;

    // The head: the title in the menus' large face over the arena select's gold bar (its ornaments kept in shape, the
    // line between them stretched), the rule of the screen under it.
    LookCentre(c, fr, Sk::kTitle, picking ? "CHOOSE THE ARENA" : roulette ? "DRAWING THE ARENA" : "THE ARENA", kLookTitle, Grey(255));
    LookBar(c, fr, P.x0 + U(fr, kLookBarInset), P.x1 - U(fr, kLookBarInset), fr.y + U(fr, kLookBarTop));
    LookCentre(c, fr, Sk::kName, "EACH PLAYER PICKS ONE ARENA. ONE OF THE TWO PICKS IS TAKEN AT RANDOM.", kLookSub, Grey(200));
    if (s.test) Sk::Write(c, Sk::kName, "TEST: SIMULATED OPPONENT", P.x0 + U(fr, 40.0f), fr.y + U(fr, 92.0f), Sk::Pm(0xFFE070, 255));
    if (picking && s.secondsLeft >= 0) {  // the countdown in a gold ring; red for the last five seconds
        char t[12];
        snprintf(t, sizeof t, "%d", std::min<int>(s.secondsLeft, 99));
        const bool late = s.secondsLeft <= 5;
        const int side = U(fr, kLookRing), x1 = P.x1 - U(fr, kLookRingRight), y0 = fr.y + U(fr, kLookRingTop);
        Sk::Piece(c, ArenaPics::kRing, x1 - side, y0, x1, y0 + side, late ? Sk::Pm(0xFF8A7A, 255) : Grey(255));
        Sk::Write(c, Sk::kLine, t, x1 - side / 2 - Sk::Width(Sk::kLine, t) / 2, y0 + side / 2 + Sk::CapOf(Sk::kLine) / 2, late ? Sk::Pm(0xFFB0A0, 255) : Grey(255));
    }

    LookTiles(c, s, l, fr, lit, myLock, theirLock);

    // The foot: the arena select's line with the sun (the sun kept in shape), then the three lines of the plain panel.
    LookSun(c, fr, cx, fr.y + U(fr, kLookSunTop), U(fr, 540.0f));
    char a[120] = "", b2[120] = "", c3[120] = "";
    uint32_t inkB = Grey(225), inkC = Grey(225);
    const uint32_t warn = Sk::Pm(0xFFE070, 255), theirsInk = Sk::Pm(0xFF9A7A, 255), mineInk = Sk::Pm(0xB4FFB0, 255);
    if (picking) {
        if (!s.myLocked) {
            snprintf(a, sizeof a, "PICK AN ARENA");
            snprintf(b2, sizeof b2, "DIRECTIONS: MOVE      CONFIRM (A / ENTER): LOCK");
        } else {
            snprintf(a, sizeof a, "YOUR PICK: %s", NameOf(s, s.myCursor));
            snprintf(b2, sizeof b2, "WAITING FOR THE OPPONENT");
        }
        if (theirLock) snprintf(c3, sizeof c3, "OPPONENT'S PICK: %s", NameOf(s, s.theirCursor)), inkC = theirsInk;
        else if (s.peer == kPeerChoosing) snprintf(c3, sizeof c3, "OPPONENT: CHOOSING");
        else if (s.peer == kPeerQuiet) snprintf(c3, sizeof c3, "OPPONENT: NO SIGNAL"), inkC = warn;
        else snprintf(c3, sizeof c3, "OPPONENT: NO ANSWER YET"), inkC = warn;
    } else if (roulette) {
        snprintf(a, sizeof a, "%s   OR   %s", NameOf(s, s.myCursor), NameOf(s, s.theirCursor));
        snprintf(b2, sizeof b2, "YOUR PICK   OR   THE OPPONENT'S PICK");
    } else {
        const bool random = s.resultCell != kNoCell && s.resultCell == s.randomCell;
        if (random) snprintf(a, sizeof a, "RANDOM: MK9 CHOOSES THE ARENA");
        else snprintf(a, sizeof a, "ARENA: %s", NameOf(s, s.resultCell));
        snprintf(b2, sizeof b2, "%s", WinnerText(s.winner));
        inkB = s.winner == kMine ? mineInk : s.winner == kTheirs ? theirsInk : Grey(255);
        if (s.note[0]) snprintf(c3, sizeof c3, "%s", s.note);
        else if (s.use == kUseTrial) snprintf(c3, sizeof c3, "TRIAL RUN: THE GAME'S OWN ARENA CHOICE STAYS");
        if (s.use == kUseTrial || s.use == kUseGameChose) inkC = warn;
    }
    // A line wider than the panel loses its end (the longest the flow writes is two thirds of it).
    const int room = P.x1 - P.x0 - 2 * U(fr, 40.0f);
    char cut[120];
    LookCentre(c, fr, Sk::kLine, FitBy([](const char* t) { return Sk::Width(Sk::kLine, t); }, a, room, cut, sizeof cut), kLookLineA, Grey(255));
    LookCentre(c, fr, Sk::kName, FitBy(NameWidth, b2, room, cut, sizeof cut), kLookLineB, inkB);
    LookCentre(c, fr, Sk::kName, FitBy(NameWidth, c3, room, cut, sizeof cut), kLookLineC, inkC);
}

void DrawPanel(Canvas& c, const State& s, const Hud::View& v, uint64_t now) {
    const Frame fr = Hud::FrameOf(v);
    const bool tiles = Tiles(s.cells, s.tiles != 0);
    const Geo& geo = tiles ? kTileGeo : kListGeo;
    const float kTitleBase = geo.titleBase, kSubBase = geo.subBase, kLineA = geo.lineA, kLineB = geo.lineB, kLineC = geo.lineC;
    const Layout l = LayoutOf(s.cells, tiles, v);
    // The game's picture is dimmed and the panel has its own dark ground: text on it needs no dark edge.
    Hud::Rect(c, v.x, v.y, v.x + v.w, v.y + v.h, Argb(0x000000, 110));
    // Nearly opaque: at 232 the menu's own lettering showed through the left column (game frames of 2026-11-12).
    Hud::Rect(c, l.panel.x0, l.panel.y0, l.panel.x1, l.panel.y1, Argb(kGround, 250));
    const int rule = std::max(1, U(fr, 2.0f));
    Hud::Rect(c, l.panel.x0, l.panel.y0, l.panel.x1, l.panel.y0 + rule, Argb(kGold, 210));
    Hud::Rect(c, l.panel.x0, l.panel.y1 - rule, l.panel.x1, l.panel.y1, Argb(kGold, 210));

    const bool picking = s.phase == kPicking, roulette = s.phase == kRoulette, result = s.phase == kResult;
    const bool theirLock = s.peer == kPeerLocked && s.theirCursor != kNoCell;
    const bool myLock = s.myLocked && s.myCursor != kNoCell;
    uint8_t lit = kNoCell;
    if (roulette && myLock && theirLock) lit = RouletteLit(s, now) == 0 ? s.myCursor : s.theirCursor;
    // The tiles first: what lies under the pictures must come before everything that lies over them.
    if (tiles) DrawTiles(c, s, l, fr, lit, myLock, theirLock);

    Centre(c, fr, kMainFace, picking ? "CHOOSE THE ARENA" : roulette ? "DRAWING THE ARENA" : "THE ARENA", kTitleBase, kWhite, false);
    Centre(c, fr, kSmallFace, "EACH PLAYER PICKS ONE ARENA. ONE OF THE TWO PICKS IS TAKEN AT RANDOM.", kSubBase, kLabel, false);
    const int pad = U(fr, 26.0f);
    if (s.test) Hud::Write(c, kSmallFace, "TEST: SIMULATED OPPONENT", l.panel.x0 + pad, fr.y + U(fr, kTitleBase), kYellow, 255, false);
    if (picking && s.secondsLeft >= 0) {
        char t[12];
        snprintf(t, sizeof t, "%d", std::min<int>(s.secondsLeft, 99));
        Hud::Write(c, kMainFace, t, l.panel.x1 - pad - Hud::Width(kMainFace, t), fr.y + U(fr, kTitleBase), s.secondsLeft <= 5 ? kRed : kWhite, 255, false);
    }

    const Dims d = DimsOf(s.cells);
    const int inset = std::max(1, U(fr, 3.0f)), textX = U(fr, 18.0f), tagX = U(fr, 12.0f), line = std::max(1, U(fr, 2.5f));
    const int base = U(fr, d.rowH * 0.70f);
    for (int i = 0; i < s.cells && !tiles; ++i) {
        const Box b{l.cell[i].x0 + inset, l.cell[i].y0 + inset, l.cell[i].x1 - inset, l.cell[i].y1 - inset};
        const bool mine = i == s.myCursor, theirs = i == s.theirCursor;
        const bool mineL = mine && myLock, theirsL = theirs && theirLock;
        uint32_t rgb = kLabel;
        int alpha = picking ? 255 : 120;  // after the picks, the cells that are not in the draw step back
        if ((result && i == s.resultCell) || (roulette && i == lit)) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(kGold, 255));
            rgb = kInk, alpha = 255;
        } else if (mineL && theirsL) {
            const int mid = (b.x0 + b.x1) / 2;
            Hud::Rect(c, b.x0, b.y0, mid, b.y1, Argb(kBarMine, picking ? 255 : 150));
            Hud::Rect(c, mid, b.y0, b.x1, b.y1, Argb(kBarTheirs, picking ? 255 : 150));
            rgb = kWhite, alpha = 255;
        } else if (mineL || theirsL) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(mineL ? kBarMine : kBarTheirs, picking ? 255 : 150));
            rgb = kWhite, alpha = picking ? 255 : 200;
        } else if (mine && picking) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(kBarCursor, 255));
            rgb = kWhite;
        }
        if (picking && mine && !s.myLocked) Outline(c, b, line, Argb(kWhite, 255));
        if (picking && theirs && !theirsL)  // the other player's cursor, live; faint where it last stood once its packets stop
            Outline(c, Box{b.x0 + line, b.y0 + line, b.x1 - line, b.y1 - line}, line, Argb(kOrange, s.peer == kPeerQuiet ? 90 : 255));
        const char* tag = mineL && theirsL ? "BOTH" : mineL ? "YOU" : theirsL ? "OPP" : "";
        const int tagW = *tag ? Hud::Width(kSmallFace, tag) + tagX : 0;
        char name[40];
        Fit(d.face, s.name[i], b.x1 - b.x0 - textX - tagW - tagX, name, sizeof name);
        Hud::Write(c, d.face, name, b.x0 + textX, l.cell[i].y0 + base, rgb, alpha, false);
        if (*tag) Hud::Write(c, kSmallFace, tag, b.x1 - tagW, l.cell[i].y0 + base, rgb == kInk ? kInk : kWhite, alpha, false);
    }

    char a[120] = "", b2[120] = "", c3[120] = "";
    uint32_t rgbA = kWhite, rgbB = kLabel, rgbC = kLabel;
    if (picking) {
        if (!s.myLocked) {
            snprintf(a, sizeof a, "PICK AN ARENA");
            snprintf(b2, sizeof b2, "DIRECTIONS: MOVE      CONFIRM (A / ENTER): LOCK");
        } else {
            snprintf(a, sizeof a, "YOUR PICK: %s", NameOf(s, s.myCursor));
            snprintf(b2, sizeof b2, "WAITING FOR THE OPPONENT");
        }
        if (theirLock) {
            snprintf(c3, sizeof c3, "OPPONENT'S PICK: %s", NameOf(s, s.theirCursor));
            rgbC = kOrange;
        } else if (s.peer == kPeerChoosing) {
            snprintf(c3, sizeof c3, "OPPONENT: CHOOSING");
        } else if (s.peer == kPeerQuiet) {
            snprintf(c3, sizeof c3, "OPPONENT: NO SIGNAL");
            rgbC = kYellow;
        } else {
            snprintf(c3, sizeof c3, "OPPONENT: NO ANSWER YET");
            rgbC = kYellow;
        }
    } else if (roulette) {
        snprintf(a, sizeof a, "%s   OR   %s", NameOf(s, s.myCursor), NameOf(s, s.theirCursor));
        snprintf(b2, sizeof b2, "YOUR PICK   OR   THE OPPONENT'S PICK");
    } else {
        const bool random = s.resultCell != kNoCell && s.resultCell == s.randomCell;
        if (random) snprintf(a, sizeof a, "RANDOM: MK9 CHOOSES THE ARENA");
        else snprintf(a, sizeof a, "ARENA: %s", NameOf(s, s.resultCell));
        snprintf(b2, sizeof b2, "%s", WinnerText(s.winner));
        rgbB = WinnerRgb(s.winner);
        if (s.note[0]) snprintf(c3, sizeof c3, "%s", s.note);
        else if (s.use == kUseTrial) snprintf(c3, sizeof c3, "TRIAL RUN: THE GAME'S OWN ARENA CHOICE STAYS");
        if (s.use == kUseTrial || s.use == kUseGameChose) rgbC = kYellow;
    }
    Centre(c, fr, kMainFace, a, kLineA, rgbA, false);
    Centre(c, fr, kSmallFace, b2, kLineB, rgbB, false);
    Centre(c, fr, kSmallFace, c3, kLineC, rgbC, false);
}

// While the match loads: two lines at the foot of the frame, on the game's own picture (so with the dark edge).
void DrawBanner(Canvas& c, const State& s, const Hud::View& v) {
    const Frame fr = Hud::FrameOf(v);
    char a[120] = "", b[120] = "";
    uint32_t rgbB = kLabel;
    if (s.use == kUseApplied && s.resultCell != kNoCell) {
        snprintf(a, sizeof a, "ARENA: %s", NameOf(s, s.resultCell));
        snprintf(b, sizeof b, "%s", WinnerText(s.winner));
        rgbB = WinnerRgb(s.winner);
    } else if (s.use == kUseTrial && s.resultCell != kNoCell) {
        snprintf(a, sizeof a, "DRAWN: %s (%s)", NameOf(s, s.resultCell), WinnerText(s.winner));
        snprintf(b, sizeof b, "%s", s.note[0] ? s.note : "TRIAL RUN: THE GAME'S OWN ARENA CHOICE STAYS");
        rgbB = kYellow;
    } else {
        snprintf(a, sizeof a, "MK9 CHOOSES THE ARENA");
        snprintf(b, sizeof b, "%s", s.note);
    }
    Centre(c, fr, kMainFace, a, kBannerA, kWhite, true);
    Centre(c, fr, kSmallFace, b, kBannerB, rgbB, true);
}

// The choice after a match (net\Refight.cpp): a small panel over MK9's own rematch box. The rows are the State's cells
// (three: RE-FIGHT, CHANGE FIGHTER OR ARENA, LEAVE), with both players' marks as on the list of arenas; 'note' is what
// the two choices gave, once it is known.
void DrawVote(Canvas& c, const State& s, const Hud::View& v) {
    const Frame fr = Hud::FrameOf(v);
    const int cx = fr.x + fr.w / 2;
    const Box panel{cx - U(fr, 430.0f), fr.y + U(fr, 300.0f), cx + U(fr, 430.0f), fr.y + U(fr, 800.0f)};
    Hud::Rect(c, v.x, v.y, v.x + v.w, v.y + v.h, Argb(0x000000, 120));
    Hud::Rect(c, panel.x0, panel.y0, panel.x1, panel.y1, Argb(kGround, 250));
    const int rule = std::max(1, U(fr, 2.0f));
    Hud::Rect(c, panel.x0, panel.y0, panel.x1, panel.y0 + rule, Argb(kGold, 210));
    Hud::Rect(c, panel.x0, panel.y1 - rule, panel.x1, panel.y1, Argb(kGold, 210));
    Centre(c, fr, kMainFace, "PLAY AGAIN?", 362.0f, kWhite, false);
    Centre(c, fr, kSmallFace, "RE-FIGHT: THE SAME FIGHTERS AND ARENA, WHEN BOTH PLAYERS CHOOSE IT.", 398.0f, kLabel, false);
    if (s.test) Hud::Write(c, kSmallFace, "TEST", panel.x0 + U(fr, 26.0f), fr.y + U(fr, 362.0f), kYellow, 255, false);
    const bool theirLock = s.peer == kPeerLocked && s.theirCursor != kNoCell, myLock = s.myLocked && s.myCursor != kNoCell;
    const bool open = !s.note[0];  // still choosing: the rows are live
    const int rowH = U(fr, 64.0f), top = fr.y + U(fr, 432.0f), half = U(fr, 330.0f), inset = std::max(1, U(fr, 4.0f));
    const int line = std::max(1, U(fr, 2.5f)), textX = U(fr, 22.0f), tagX = U(fr, 14.0f), base = U(fr, 43.0f);
    for (int i = 0; i < s.cells && i < 4; ++i) {
        const Box b{cx - half, top + i * rowH + inset, cx + half, top + (i + 1) * rowH - inset};
        const bool mine = i == s.myCursor, theirs = i == s.theirCursor, mineL = mine && myLock, theirsL = theirs && theirLock;
        uint32_t rgb = kLabel;
        int alpha = open ? 255 : 130;
        if (mineL && theirsL) {
            const int mid = (b.x0 + b.x1) / 2;
            Hud::Rect(c, b.x0, b.y0, mid, b.y1, Argb(kBarMine, 255));
            Hud::Rect(c, mid, b.y0, b.x1, b.y1, Argb(kBarTheirs, 255));
            rgb = kWhite, alpha = 255;
        } else if (mineL || theirsL) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(mineL ? kBarMine : kBarTheirs, 255));
            rgb = kWhite, alpha = 255;
        } else if (mine && open) {
            Hud::Rect(c, b.x0, b.y0, b.x1, b.y1, Argb(kBarCursor, 255));
            rgb = kWhite;
        }
        if (open && mine && !s.myLocked) Outline(c, b, line, Argb(kWhite, 255));
        if (open && theirs && !theirsL) Outline(c, Box{b.x0 + line, b.y0 + line, b.x1 - line, b.y1 - line}, line, Argb(kOrange, s.peer == kPeerQuiet ? 90 : 255));
        const char* tag = mineL && theirsL ? "BOTH" : mineL ? "YOU" : theirsL ? "OPP" : "";
        const int tagW = *tag ? Hud::Width(kSmallFace, tag) + tagX : 0;
        char name[40];
        Fit(kMainFace, s.name[i], b.x1 - b.x0 - textX - tagW - tagX, name, sizeof name);
        Hud::Write(c, kMainFace, name, b.x0 + textX, b.y0 - inset + base, rgb, alpha, false);
        if (*tag) Hud::Write(c, kSmallFace, tag, b.x1 - tagW, b.y0 - inset + base, kWhite, alpha, false);
    }
    char a[120] = "", b2[120] = "", c3[120] = "";
    uint32_t rgbA = kWhite, rgbC = kLabel;
    if (!open) {
        snprintf(a, sizeof a, "%s", s.note);
        rgbA = kGold;
    } else if (!s.myLocked) {
        snprintf(a, sizeof a, "CHOOSE");
        snprintf(b2, sizeof b2, "UP / DOWN: MOVE      CONFIRM (A / ENTER): LOCK");
    } else {
        snprintf(a, sizeof a, "YOUR CHOICE: %s", NameOf(s, s.myCursor));
        snprintf(b2, sizeof b2, "WAITING FOR THE OPPONENT      BACK (B): LEAVE");
    }
    if (theirLock) {
        snprintf(c3, sizeof c3, "OPPONENT'S CHOICE: %s", NameOf(s, s.theirCursor));
        rgbC = kOrange;
    } else if (!open) {
        c3[0] = 0;
    } else if (s.peer == kPeerChoosing) {
        snprintf(c3, sizeof c3, "OPPONENT: CHOOSING");
    } else if (s.peer == kPeerQuiet) {
        snprintf(c3, sizeof c3, "OPPONENT: NO SIGNAL");
        rgbC = kYellow;
    } else {
        snprintf(c3, sizeof c3, "OPPONENT: NO ANSWER YET");
        rgbC = kYellow;
    }
    Centre(c, fr, kMainFace, a, 676.0f, rgbA, false);
    Centre(c, fr, kSmallFace, b2, 718.0f, kLabel, false);
    Centre(c, fr, kSmallFace, c3, 756.0f, rgbC, false);
}

// A re-fight both players chose, on the character select: two lines at the foot of the frame, on the game's own
// picture (name[0], then the note).
void DrawCard(Canvas& c, const State& s, const Hud::View& v) {
    const Frame fr = Hud::FrameOf(v);
    Centre(c, fr, kMainFace, s.name[0], kBannerA, kGold, true);
    Centre(c, fr, kSmallFace, s.note, kBannerB, kWhite, true);
}

// ---- the choice after a match and the re-fight's card in MK9's look ----
// The same panel as DrawVote, drawn like the arena grid: the game's menu lettering, the gold bar under the title, the
// line with the sun over the status lines. Each choice is a plate in a column, marked the way a tile is: thin gold
// around it, bright and lit under this player's cursor, a pick in its player's colour with the player's tag at its
// end (this player's at the left, the other's at the right), the other player's live cursor red inside it.
constexpr int kVoteLookRows = 3;  // more rows than this: the plain panel
constexpr float kVoteHalfW = 480.0f, kVoteTop = 250.0f, kVoteBottom = 826.0f;
constexpr float kVoteTitle = 306.0f, kVoteBarTop = 294.0f, kVoteBarInset = 70.0f, kVoteSub = 357.0f;
constexpr float kVoteRowTop = 388.0f, kVoteRowH = 80.0f, kVoteRowHalfW = 350.0f, kVoteRowInset = 9.0f;
constexpr float kVoteSunTop = 646.0f, kVoteLineA = 726.0f, kVoteLineB = 760.0f, kVoteLineC = 791.0f;

void DrawVoteLook(Sk::Canvas& c, const State& s, const Hud::View& v) {
    const Frame fr = Hud::FrameOf(v);
    const int cx = fr.x + fr.w / 2;
    const Box P{cx - U(fr, kVoteHalfW), fr.y + U(fr, kVoteTop), cx + U(fr, kVoteHalfW), fr.y + U(fr, kVoteBottom)};
    const int room = P.x1 - P.x0 - 2 * U(fr, 40.0f);
    char cut[120];
    const auto lineWidth = [](const char* t) { return Sk::Width(Sk::kLine, t); };
    LookGround(c, fr, P, cx, 440.0f);
    LookCentre(c, fr, Sk::kTitle, "PLAY AGAIN?", kVoteTitle, Grey(255));
    LookBar(c, fr, P.x0 + U(fr, kVoteBarInset), P.x1 - U(fr, kVoteBarInset), fr.y + U(fr, kVoteBarTop));
    LookCentre(c, fr, Sk::kName, FitBy(NameWidth, "RE-FIGHT: THE SAME FIGHTERS AND ARENA, WHEN BOTH PLAYERS CHOOSE IT.", room, cut, sizeof cut), kVoteSub, Grey(200));
    if (s.test) Sk::Write(c, Sk::kName, "TEST", P.x0 + U(fr, 40.0f), P.y0 + U(fr, 36.0f), Sk::Pm(0xFFE070, 255));

    const bool theirLock = s.peer == kPeerLocked && s.theirCursor != kNoCell, myLock = s.myLocked && s.myCursor != kNoCell;
    const bool open = !s.note[0];  // still choosing: the rows are live
    const int thin = std::max(1, U(fr, 2.0f)), mid = std::max(2, U(fr, 3.0f)), thick = std::max(2, U(fr, 4.0f));
    const int rowH = U(fr, kVoteRowH), top = fr.y + U(fr, kVoteRowTop), half = U(fr, kVoteRowHalfW), inset = U(fr, kVoteRowInset);
    const int capLine = Sk::CapOf(Sk::kLine), capName = Sk::CapOf(Sk::kName);
    const int tagW = std::max(Sk::Width(Sk::kName, "YOU"), Sk::Width(Sk::kName, "OPP")) + 2 * U(fr, 14.0f);
    const int rows = std::min<int>(s.cells, kVoteLookRows);
    struct Row {
        Box b;
        bool mineL, theirsL, theirs, cursor, agreed;
    };
    const auto rowOf = [&](int i) {
        Row k;
        k.b = Box{cx - half, top + i * rowH + inset, cx + half, top + (i + 1) * rowH - inset};
        k.mineL = i == s.myCursor && myLock, k.theirsL = i == s.theirCursor && theirLock;
        k.theirs = open && i == s.theirCursor && !k.theirsL;       // the other player's cursor, live
        k.cursor = open && i == s.myCursor && !s.myLocked;         // this player's cursor
        k.agreed = !open && k.mineL && k.theirsL;                  // what both chose, once the choice is over
        return k;
    };
    // Under the plates: each one's black edge, then the light behind this player's cursor and behind what both chose.
    for (int i = 0; i < rows; ++i) {
        const Box b = rowOf(i).b;
        Sk::Fill(c, b.x0 - thick - 1, b.y0 - thick - 1, b.x1 + thick + 1, b.y1 + thick + 1, Sk::Pm(0x000000, 215));
    }
    for (int i = 0; i < rows; ++i) {
        const Row k = rowOf(i);
        if (!k.cursor && !k.agreed) continue;
        Sk::Glow(c, ArenaPics::kHalo, k.b.x0, k.b.y0, k.b.x1, k.b.y1, std::max(3, U(fr, k.agreed ? 16.0f : 13.0f)), Sk::Lit(k.agreed ? kGoldLight : kEmber, k.agreed ? 235 : 200));
    }
    for (int i = 0; i < rows; ++i) {
        const Row k = rowOf(i);
        const Box& b = k.b;
        const bool lit = k.cursor || k.agreed, picked = k.mineL || k.theirsL;
        // How bright the row is: full under the cursor and for a pick; once the choice is over the others step far back.
        const int level = lit || picked ? 255 : open ? 205 : 80;
        if (lit) Sk::FillV(c, b.x0, b.y0, b.x1, b.y1, Sk::Pm(0x5C1710, 255), Sk::Pm(0x1C0605, 255));
        else if (level > 100) Sk::FillV(c, b.x0, b.y0, b.x1, b.y1, Sk::Pm(0x3A0D0A, 255), Sk::Pm(0x0E0303, 255));
        else Sk::FillV(c, b.x0, b.y0, b.x1, b.y1, Sk::Pm(0x1A0705, 255), Sk::Pm(0x080202, 255));
        if (k.agreed) LookFrame(c, b, thick + 1, Sk::Pm(0xFFFFFF, 255), Sk::Pm(kGoldLight, 255));
        else if (k.mineL && k.theirsL) LookFrame(c, b, mid, Sk::Pm(0xFFE38A, 255), Sk::Pm(kBlood, 255));
        else if (k.mineL) LookFrame(c, b, mid, Sk::Pm(0xFFE38A, 255), Sk::Pm(0xD9A93A, 255));
        else if (k.theirsL) LookFrame(c, b, mid, Sk::Pm(0xFF5A46, 255), Sk::Pm(kBloodDark, 255));
        else if (k.cursor) LookFrame(c, b, thick, Sk::Pm(0xFFF6D8, 255), Sk::Pm(0xF0C060, 255));
        else LookFrame(c, b, thin, Sk::Pm(kGoldMid, level), Sk::Pm(kGoldDark, level));
        const int base = (b.y0 + b.y1) / 2, tagBase = base + capName / 2;
        if (lit) {  // the menus' glow behind the text: two halves, brightest where they meet
            const uint32_t glow = Sk::Lit(0xFFFFFF, k.agreed ? 125 : 100);  // weaker than under a tile's small name: these letters are orange themselves
            Sk::Piece(c, ArenaPics::kGlow, b.x0 + tagW, b.y0 + thin, cx, b.y1 - thin, glow);
            Sk::Piece(c, ArenaPics::kGlow, cx, b.y0 + thin, b.x1 - tagW, b.y1 - thin, glow, true);
        }
        if (k.theirs) {  // red, inside the plate, with the tag in red; faint once its packets stop
            const int alpha = s.peer == kPeerQuiet ? 90 : 255;
            const uint32_t red = Sk::Pm(kBlood, alpha);
            Sk::Fill(c, b.x0, b.y0, b.x1, b.y0 + mid, red);
            Sk::Fill(c, b.x0, b.y1 - mid, b.x1, b.y1, red);
            Sk::Fill(c, b.x0, b.y0 + mid, b.x0 + mid, b.y1 - mid, red);
            Sk::Fill(c, b.x1 - mid, b.y0 + mid, b.x1, b.y1 - mid, red);
            Sk::Write(c, Sk::kName, "OPP", b.x1 - tagW / 2 - Sk::Width(Sk::kName, "OPP") / 2, tagBase, Sk::Pm(0xFF9A7A, alpha));
        }
        if (k.mineL) {  // whose pick: a dark end of the plate, its inner edge in the player's colour
            Sk::Fill(c, b.x0, b.y0, b.x0 + tagW, b.y1, Sk::Pm(0x000000, 200));
            Sk::Fill(c, b.x0 + tagW, b.y0, b.x0 + tagW + thin, b.y1, Sk::Pm(0xFFE38A, 255));
            Sk::Write(c, Sk::kName, "YOU", b.x0 + tagW / 2 - Sk::Width(Sk::kName, "YOU") / 2, tagBase, Grey(255));
        }
        if (k.theirsL) {
            Sk::Fill(c, b.x1 - tagW, b.y0, b.x1, b.y1, Sk::Pm(0x000000, 200));
            Sk::Fill(c, b.x1 - tagW - thin, b.y0, b.x1 - tagW, b.y1, Sk::Pm(kBlood, 255));
            Sk::Write(c, Sk::kName, "OPP", b.x1 - tagW / 2 - Sk::Width(Sk::kName, "OPP") / 2, tagBase, Grey(255));
        }
        char name[40];
        FitBy(lineWidth, s.name[i], b.x1 - b.x0 - 2 * (tagW + thick), name, sizeof name);
        // The letters' own black shape under them, a little to every side: they stay sharp on the glow.
        const int tx = cx - Sk::Width(Sk::kLine, name) / 2, ty = base + capLine / 2, d = std::max(1, U(fr, 1.5f));
        const uint32_t shade = Sk::Pm(0x000000, lit ? 200 : 130);
        const int off[4][2] = {{-d, -d}, {d, -d}, {-d, d}, {d, d}};
        for (const auto& o : off) Sk::Write(c, Sk::kLine, name, tx + o[0], ty + o[1], shade);
        Sk::Write(c, Sk::kLine, name, tx, ty, Grey(level == 255 ? 255 : level == 205 ? 215 : 120));
    }

    LookSun(c, fr, cx, fr.y + U(fr, kVoteSunTop), U(fr, 380.0f));
    char a[120] = "", b2[120] = "", c3[120] = "";
    uint32_t inkA = Grey(255), inkC = Grey(225);
    const uint32_t warn = Sk::Pm(0xFFE070, 255), theirsInk = Sk::Pm(0xFF9A7A, 255);
    if (!open) {
        snprintf(a, sizeof a, "%s", s.note);
        inkA = Sk::Pm(kGoldLight, 255);
    } else if (!s.myLocked) {
        snprintf(a, sizeof a, "CHOOSE");
        snprintf(b2, sizeof b2, "UP / DOWN: MOVE      CONFIRM (A / ENTER): LOCK");
    } else {
        snprintf(a, sizeof a, "YOUR CHOICE: %s", NameOf(s, s.myCursor));
        snprintf(b2, sizeof b2, "WAITING FOR THE OPPONENT      BACK (B): LEAVE");
    }
    if (theirLock) snprintf(c3, sizeof c3, "OPPONENT'S CHOICE: %s", NameOf(s, s.theirCursor)), inkC = theirsInk;
    else if (!open) c3[0] = 0;
    else if (s.peer == kPeerChoosing) snprintf(c3, sizeof c3, "OPPONENT: CHOOSING");
    else if (s.peer == kPeerQuiet) snprintf(c3, sizeof c3, "OPPONENT: NO SIGNAL"), inkC = warn;
    else snprintf(c3, sizeof c3, "OPPONENT: NO ANSWER YET"), inkC = warn;
    LookCentre(c, fr, Sk::kLine, FitBy(lineWidth, a, room, cut, sizeof cut), kVoteLineA, inkA);
    LookCentre(c, fr, Sk::kName, FitBy(NameWidth, b2, room, cut, sizeof cut), kVoteLineB, Grey(225));
    // Once the choice is over there is no second line: the opponent's choice stands in its place.
    LookCentre(c, fr, Sk::kName, FitBy(NameWidth, c3, room, cut, sizeof cut), b2[0] ? kVoteLineC : kVoteLineB, inkC);
}

// The re-fight's card: a dark band at the foot of the frame, soft at its ends, between two gold rules, and the two
// lines in the game's lettering. The band's middle is a plain rectangle (the display draws the look only over a plain
// list, Overlay.cpp); its ends, the rules and the lettering come from the texture.
constexpr float kCardTop = 938.0f, kCardBottom = 1038.0f, kCardHalfW = 600.0f, kCardCore = 280.0f;
constexpr int kCardShade = 190;

void DrawCardLook(Canvas& plain, Sk::Canvas& c, const State& s, const Hud::View& v) {
    const Frame fr = Hud::FrameOf(v);
    const int cx = fr.x + fr.w / 2, y0 = fr.y + U(fr, kCardTop), y1 = fr.y + U(fr, kCardBottom);
    const int out = std::min(U(fr, kCardHalfW), fr.w / 2), core = std::min(U(fr, kCardCore), out), rule = std::max(1, U(fr, 2.0f));
    Hud::Rect(plain, cx - core, y0, cx + core, y1, Argb(0x000000, kCardShade));
    Sk::FillH(c, cx - out, y0, cx - core, y1, Sk::Pm(0x000000, 0), Sk::Pm(0x000000, kCardShade));
    Sk::FillH(c, cx + core, y0, cx + out, y1, Sk::Pm(0x000000, kCardShade), Sk::Pm(0x000000, 0));
    LookRule(c, cx - out, cx + out, y0, y0 + rule, kGoldLight, 255);
    LookRule(c, cx - out, cx + out, y0 + rule, y0 + 2 * rule, kGoldDark, 255);
    LookRule(c, cx - out, cx + out, y1 - 2 * rule, y1 - rule, kGoldLight, 255);
    LookRule(c, cx - out, cx + out, y1 - rule, y1, kGoldDark, 255);
    char cut[120];
    const int room = 2 * out - 2 * U(fr, 60.0f);
    LookCentre(c, fr, Sk::kLine, FitBy([](const char* t) { return Sk::Width(Sk::kLine, t); }, s.name[0], room, cut, sizeof cut), kBannerA, Sk::Pm(kGoldLight, 255));
    LookCentre(c, fr, Sk::kName, FitBy(NameWidth, s.note, room, cut, sizeof cut), kBannerB, Grey(240));
}

State gBuf[2];
volatile LONG gFront = 0, gDemo = 0, gFaces = 0, gSeenTick = 0;
volatile LONG gDraws = 0, gLargest = 0, gDroppedTotal = 0, gLookDraws = 0, gLookLargest = 0;
int gDropped = 0;

// The stage table as the game listed it ('stages'): the 28 entries a player may pick. For the canned states only.
const char* const kDemoNames[] = {
    "BGND_Courtyard", "BGND_CourtyardN", "BGND_DeadPool",    "BGND_Desert",      "BGND_FleshPits",   "BGND_GorosLair", "BGND_Graveyard",
    "BGND_Hell",      "BGND_KahnsColiseum", "BGND_LivingForest", "BGND_Pit",     "BGND_PitBottom",   "BGND_PitDay",    "BGND_RooftopDawn",
    "BGND_RooftopDay", "BGND_RooftopDusk", "BGND_SKThrone",  "BGND_SoulChamber", "BGND_STGardens",   "BGND_STGardensN", "BGND_Street",
    "BGND_STThrone",  "BGND_Subway",     "BGND_Temple",      "BGND_Tower",       "BGND_Wastelands",  "BGND_Armory",    "BGND_BellTower"};

bool Word(const char* w, int n, const char* is) { return (int)strlen(is) == n && !_strnicmp(w, is, (size_t)n); }
}  // namespace

namespace StageScreen {

void DisplayName(const char* tableName, char* out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    const char* s = tableName ? tableName : "";
    if (!_strnicmp(s, "BGND_", 5)) s += 5;
    int n = 0;
    const auto put = [&](const char* text, int len) {
        if (n && n < cap - 1) out[n++] = ' ';
        for (int i = 0; i < len && n < cap - 1; ++i) {
            const char ch = text[i];
            out[n++] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
        }
        out[n] = 0;
    };
    const auto upper = [](char ch) { return ch >= 'A' && ch <= 'Z'; };
    const auto lower = [](char ch) { return ch >= 'a' && ch <= 'z'; };
    // "ST" and "SK" before a capitalized word are the two bosses.
    if (s[0] == 'S' && (s[1] == 'T' || s[1] == 'K') && upper(s[2]) && lower(s[3])) {
        const char* who = s[1] == 'T' ? "SHANG TSUNG" : "SHAO KAHN";
        put(who, (int)strlen(who));
        s += 2;
    }
    while (*s) {
        while (*s == '_' || *s == ' ') ++s;
        if (!*s) break;
        const char* w = s++;
        // A word ends before a capital that follows a small letter or a digit, or that starts a new capitalized word.
        while (*s && *s != '_' && *s != ' ' && !(upper(*s) && (!upper(s[-1]) || lower(s[1])))) ++s;
        const int len = (int)(s - w);
        const bool last = !*s;
        if (last && n && Word(w, len, "N")) put("NIGHT", 5);
        else if (Word(w, len, "Kahns")) put("KAHN'S", 6);
        else if (Word(w, len, "Goros")) put("GORO'S", 6);
        else put(w, len);
    }
    if (!n) snprintf(out, (size_t)cap, "?");
}

bool Tiles(int cells, bool wanted) { return wanted && cells >= 1 && cells <= kTileCells; }

Layout LayoutOf(int cells, bool tiles, const Hud::View& v) {
    Layout l;
    cells = std::clamp(cells, 0, kMaxCells);
    tiles = Tiles(cells, tiles);
    const Geo& geo = tiles ? kTileGeo : kListGeo;
    const Frame fr = Hud::FrameOf(v);
    l.frame = Box{fr.x, fr.y, fr.x + fr.w, fr.y + fr.h};
    const Dims d = DimsOf(cells);
    l.tiles = tiles;
    l.cols = tiles ? kTileCols : d.cols;
    l.rows = (cells + l.cols - 1) / l.cols;
    l.face = tiles ? kSmallFace : d.face;
    const int cx = fr.x + fr.w / 2;
    l.panel = Box{cx - U(fr, geo.halfW), fr.y + U(fr, geo.top), cx + U(fr, geo.halfW), fr.y + U(fr, geo.bottom)};
    const int cw = U(fr, tiles ? kTileW : d.cellW), rh = U(fr, tiles ? kTileH : d.rowH);
    const int gx = cx - cw * l.cols / 2, gy = fr.y + U(fr, geo.gridTop);
    // A tile's picture: as wide as the tile allows, 16:9.
    const int in = U(fr, kTilePicInset), pw = cw - 2 * in, ph = (pw * 9 + 8) / 16;
    for (int i = 0; i < cells; ++i) {
        const int col = i % l.cols, row = i / l.cols;
        l.cell[i] = Box{gx + col * cw, gy + row * rh, gx + (col + 1) * cw, gy + (row + 1) * rh};
        if (tiles) l.pic[i] = Box{l.cell[i].x0 + in, l.cell[i].y0 + in, l.cell[i].x0 + in + pw, l.cell[i].y0 + in + ph};
    }
    return l;
}

int Wrap(Hud::FaceId f, const char* text, int maxW, char* a, char* b, int cap) {
    return WrapBy([f](const char* t) { return Hud::Width(f, t); }, text, maxW, a, b, cap);
}
int WrapLook(const char* text, int maxW, char* a, char* b, int cap) { return WrapBy(NameWidth, text, maxW, a, b, cap); }
int LookNameRoom(const Layout& l, const Hud::View& v) { return LookRoom(l, Hud::FrameOf(v)); }

int Pictures(const Pic** list, int* split) {
    if (list) *list = gPics;
    if (split) *split = gSplit;
    return gPicN;
}
int Look(const StageSkin::Vtx** v) {
    if (v) *v = gLookV;
    return gLookN;
}

uint8_t Move(int cells, bool tiles, uint8_t from, int dx, int dy) {
    cells = std::clamp(cells, 0, kMaxCells);
    if (cells <= 0) return kNoCell;
    if (from >= cells) return (uint8_t)(cells - 1);
    const int cols = Tiles(cells, tiles) ? kTileCols : DimsOf(cells).cols, rows = (cells + cols - 1) / cols;
    int col = from % cols, row = from / cols;
    dx = dx > 0 ? 1 : dx < 0 ? -1 : 0;
    dy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    for (int k = 0; dx && k < cols; ++k) {
        col = (col + dx + cols) % cols;
        if (row * cols + col < cells) break;
    }
    for (int k = 0; dy && k < rows; ++k) {
        row = (row + dy + rows) % rows;
        if (row * cols + col < cells) break;
    }
    return (uint8_t)(row * cols + col);
}

int RouletteLit(const State& s, uint64_t nowMs) {
    const int final = s.winner == kTheirs ? 1 : 0;
    const uint64_t t = nowMs > s.phaseSinceMs ? nowMs - s.phaseSinceMs : 0;
    if (t >= kRouletteMs) return final;
    // The lit pick changes 9, 11 or 13 times, fast at first and slower towards the end, and the last change lands on
    // the winner.
    const int flips = 9 + 2 * (int)(s.seed % 3);
    const float x = (float)t / (float)kRouletteMs, eased = 1.0f - (1.0f - x) * (1.0f - x);
    const int done = std::min(flips, (int)(eased * ((float)flips + 0.5f)));
    return final ^ ((flips - done) & 1);
}

int BuildState(const State& given, const Hud::View& v, uint64_t nowMs, Hud::Vtx* out, int cap) {
    gDropped = 0;
    gPicN = gSplit = gLookN = 0;
    if (given.phase == kOff || !Hud::FacesFor(v)) return 0;
    static State s;  // rendering thread only (the host test is one thread)
    s = given;
    Sanitize(s);
    if (s.phase == kOff) return 0;
    Canvas c{out, 0, cap, 0};
    if (s.phase == kBanner) {
        DrawBanner(c, s, v);
    } else if (s.phase == kVote && s.cells <= kVoteLookRows && StageSkin::ReadyFor(v)) {
        // MK9's look, as for the grid: the game's picture dimmed as a plain rectangle, the panel from the texture.
        Hud::Rect(c, v.x, v.y, v.x + v.w, v.y + v.h, Argb(0x000000, 140));
        Sk::Canvas look{gLookV, 0, kLookMaxV, 0};
        DrawVoteLook(look, s, v);
        gLookN = look.n;
        c.dropped += look.dropped;
    } else if (s.phase == kVote) {
        DrawVote(c, s, v);
    } else if (s.phase == kCard && StageSkin::ReadyFor(v)) {
        Sk::Canvas look{gLookV, 0, kLookMaxV, 0};
        DrawCardLook(c, look, s, v);
        gLookN = look.n;
        c.dropped += look.dropped;
    } else if (s.phase == kCard) {
        DrawCard(c, s, v);
    } else if (Tiles(s.cells, s.tiles != 0) && StageSkin::ReadyFor(v)) {
        // MK9's look: the game's picture dimmed as a plain rectangle, everything else from the one texture.
        Hud::Rect(c, v.x, v.y, v.x + v.w, v.y + v.h, Argb(0x000000, 140));
        Sk::Canvas look{gLookV, 0, kLookMaxV, 0};
        DrawLook(look, s, v, nowMs);
        gLookN = look.n;
        c.dropped += look.dropped;
    } else {
        DrawPanel(c, s, v, nowMs);
    }
    gDropped = c.dropped;
    return c.n;
}

int Dropped() { return gDropped; }

void Publish(const State& s) {
    const LONG back = (gFront & 1) ^ 1;
    gBuf[back] = s;
    InterlockedExchange(&gFront, back);
}

bool CanDraw() { return gFaces && (LONG)GetTickCount() - gSeenTick < 1000; }

int Build(const Hud::View& v, bool faces, uint64_t nowMs, Hud::Vtx* out, int cap) {
    InterlockedExchange(&gFaces, faces ? 1 : 0);
    InterlockedExchange(&gSeenTick, (LONG)GetTickCount());
    if (!faces) return 0;
    static State s;  // rendering thread only
    const int demo = gDemo;
    gPicN = gSplit = gLookN = 0;
    if (demo > 0) {
        if (!Demo(demo, nowMs, &s)) return 0;
    } else {
        if (gBuf[gFront & 1].phase == kOff) return 0;  // the usual case: nothing copied
        s = gBuf[gFront & 1];
    }
    // MK9's look: its lettering is scaled for this picture size when the panel first shows tiles (and again after a
    // change of the size); without the game's art, or with the look switched off, the plain tiles are drawn. The
    // choice after a match and the re-fight's card take the look the same way, whatever the grid's form was.
    if ((s.tiles && s.phase != kBanner) || s.phase == kVote || s.phase == kCard) StageSkin::Prepare(ArenaPics::ArtOf(), v);
    const int n = BuildState(s, v, nowMs, out, cap);
    if (n >= 3) InterlockedIncrement(&gDraws);
    if (gLookN >= 3) InterlockedIncrement(&gLookDraws);
    if (gLookN > gLookLargest) InterlockedExchange(&gLookLargest, gLookN);
    if (n > gLargest) InterlockedExchange(&gLargest, n);
    if (gDropped) InterlockedExchangeAdd(&gDroppedTotal, gDropped);
    return n;
}

void Counters(long* draws, long* largest, long* dropped) {
    *draws = gDraws;
    *largest = gLargest;
    *dropped = gDroppedTotal;
}
void LookCounters(long* draws, long* largest) {
    *draws = gLookDraws;
    *largest = gLookLargest;
}

void SetDemo(int n) { InterlockedExchange(&gDemo, n < 0 || n > kDemos + kVoteDemos ? 0 : n); }
int DemoShown() { return gDemo; }

// The choice after a match and the re-fight's card (canned states kDemos + 1 ...).
static bool DemoVote(int n, State* s) {
    static const char* const rows[] = {"RE-FIGHT", "CHANGE FIGHTER OR ARENA", "LEAVE"};
    for (int i = 0; i < 3; ++i) snprintf(s->name[i], sizeof s->name[i], "%s", rows[i]);
    s->cells = 3;
    s->phase = kVote;
    s->myCursor = 0, s->theirCursor = 1, s->peer = kPeerChoosing;
    switch (n) {
    case 1:  // both choosing
        break;
    case 2:  // this player has locked RE-FIGHT, the opponent's cursor stands on it too
        s->myLocked = 1, s->theirCursor = 0;
        break;
    case 3:  // both chose RE-FIGHT
        s->myLocked = 1, s->theirCursor = 0, s->peer = kPeerLocked;
        snprintf(s->note, sizeof s->note, "RE-FIGHT: THE SAME FIGHTERS, THE SAME ARENA");
        break;
    case 4:  // one wants a change
        s->myLocked = 1, s->theirCursor = 1, s->peer = kPeerLocked;
        snprintf(s->note, sizeof s->note, "BACK TO THE CHARACTER SELECT");
        break;
    case 5:  // the card on the character select
        *s = State{};
        s->phase = kCard;
        snprintf(s->name[0], sizeof s->name[0], "RE-FIGHT");
        snprintf(s->note, sizeof s->note, "THE SAME ARENA AGAIN. CONFIRM YOUR FIGHTER.");
        break;
    default:
        return false;
    }
    return true;
}

bool Demo(int n, uint64_t nowMs, State* s) {
    *s = State{};
    if (n > kDemos) return DemoVote(n - kDemos, s);
    if (n < 1 || n > kDemos) return false;
    const int stages = (int)(sizeof kDemoNames / sizeof kDemoNames[0]);
    for (int i = 0; i < stages; ++i) DisplayName(kDemoNames[i], s->name[i], sizeof s->name[i]);
    // Picture tiles where the game's pictures were read (the same rule as the flow's).
    s->tiles = ArenaPics::Usable() ? 1 : 0;
    for (int i = 0; i < stages && s->tiles; ++i) s->pic[i] = (uint8_t)(ArenaPics::SlotOf(kDemoNames[i]) + 1);
    snprintf(s->name[stages], sizeof s->name[stages], "RANDOM");
    s->cells = (uint8_t)(stages + 1);
    s->randomCell = (uint8_t)stages;
    s->phase = kPicking;
    s->myCursor = 10, s->theirCursor = 3;
    s->peer = kPeerChoosing;
    s->secondsLeft = 12;
    s->phaseSinceMs = nowMs;
    s->seed = 0x5EED;
    const auto drawn = [&](uint8_t phase, uint8_t winner, uint8_t mine, uint8_t theirs) {
        s->phase = phase, s->winner = winner;
        s->myCursor = mine, s->theirCursor = theirs;
        s->myLocked = 1, s->peer = kPeerLocked;
        s->resultCell = winner == kTheirs ? theirs : mine;
        s->secondsLeft = -1;
    };
    switch (n) {
    case 1:  // both choosing
        break;
    case 2:  // this player has locked
        s->myLocked = 1, s->secondsLeft = 9;
        break;
    case 3:  // the opponent has locked, the countdown is short
        s->peer = kPeerLocked, s->theirCursor = 7, s->secondsLeft = 4;
        break;
    case 4:  // the roulette, 0.7 s in
        drawn(kRoulette, kTheirs, 10, 7);
        s->phaseSinceMs = nowMs - 700;
        break;
    case 5:
        drawn(kResult, kMine, 10, 7);
        s->use = kUseApplied;
        break;
    case 6:
        drawn(kResult, kTheirs, 10, 7);
        s->use = kUseApplied;
        break;
    case 7:  // the same pick twice
        drawn(kResult, kBoth, 10, 10);
        s->use = kUseApplied;
        break;
    case 8:  // "random" won, in a trial run
        drawn(kResult, kMine, (uint8_t)stages, 19);
        s->use = kUseTrial;
        break;
    case 9:  // while the match loads
        drawn(kBanner, kTheirs, 10, 19);
        s->use = kUseApplied;
        break;
    case 10:  // a fallback, while the match loads
        s->phase = kBanner, s->use = kUseGameChose;
        snprintf(s->note, sizeof s->note, "THE OPPONENT DID NOT PICK IN TIME");
        break;
    case 11:  // just opened: the cursor on RANDOM, nothing heard from the opponent, the simulated opponent's mark
        s->myCursor = (uint8_t)stages, s->theirCursor = kNoCell, s->peer = kPeerSilent, s->test = 1, s->secondsLeft = 20;
        break;
    case 12:  // the largest grid: 47 stages and RANDOM, the longest name in every cell that has no real one
        for (int i = stages; i < kMaxCells - 1; ++i) snprintf(s->name[i], sizeof s->name[i], "SHANG TSUNG GARDENS NIGHT %d", i);
        snprintf(s->name[kMaxCells - 1], sizeof s->name[0], "RANDOM");
        s->cells = kMaxCells, s->randomCell = kMaxCells - 1;
        s->myCursor = 40, s->theirCursor = 17, s->peer = kPeerLocked, s->myLocked = 1, s->secondsLeft = 88;
        break;
    case 13:  // the opponent's packets have stopped: its cursor faint where it last stood
        s->peer = kPeerQuiet, s->secondsLeft = 7;
        break;
    }
    return true;
}

}  // namespace StageScreen
