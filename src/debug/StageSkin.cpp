#include "StageSkin.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
using namespace StageSkin;
using ArenaPics::kAtlasH;
using ArenaPics::kAtlasW;
using ArenaPics::kCodes;
using ArenaPics::kFirstCode;

struct Spec {
    ArenaPics::FontId font;
    float capUnits;    // a capital's height, in 1/1080 of the frame's height
    const char* only;  // the signs this face is asked for (nullptr: all)
};
// The title face is the largest and only ever writes the panel's headings and the mark of "random".
constexpr Spec kSpec[kFaces] = {{ArenaPics::kLargeMenu, 44.0f, "ABCDEFGHIJKLMNOPQRSTUVWXYZ?"}, {ArenaPics::kLargeMenu, 27.0f, nullptr}, {ArenaPics::kSmallMenu, 17.0f, nullptr}};

constexpr int kFineBelow = 15;  // capitals lower than this many pixels: letters placed between pixels (Write)

struct Letter {
    uint16_t x = 0, y = 0, w = 0, h = 0;  // in the atlas (w 0: nothing drawn)
    int16_t dx = 0, dy = 0;               // from the pen on the baseline to the box's top left
    float advance = 0;
};
struct FaceData {
    Letter letter[kCodes];
    const int16_t (*kern)[kCodes] = nullptr;
    float kernPx = 0;  // pixels per 1/20480 EM
    int cap = 0;
};
FaceData gFace[kFaces];
const ArenaPics::Art* gArt = nullptr;
int gForW = 0, gForH = 0, gFailW = 0, gFailH = 0;
const ArenaPics::Art* gFailArt = nullptr;
bool gReady = false;
uint32_t gVersion = 0;
char gText[200] = "no faces built";

// The lower half of the atlas, filled in rows.
struct Packer {
    int x = 0, y = kAtlasH / 2, rowH = 0;
    bool Take(int w, int h, int* px, int* py) {
        if (w + 1 > kAtlasW) return false;
        if (x + w + 1 > kAtlasW) x = 0, y += rowH + 1, rowH = 0;
        if (y + h + 1 > kAtlasH) return false;
        *px = x, *py = y;
        x += w + 1;
        rowH = std::max(rowH, h);
        return true;
    }
};

// One letter at a scale. The pen's place in the scaled box is a whole pixel (ox, oy rounded up), so every letter of a
// line stands on the same row of pixels. Below scale 1 a pixel is the average of the sheet's pixels under it (the
// colours are already multiplied by alpha, so the average is right at a letter's edge); above, between the four nearest.
bool ScaleLetter(const ArenaPics::Font& f, const ArenaPics::Glyph& g, float s, uint32_t* atlas, Packer& pack, Letter* out) {
    const int ox = (int)std::ceil(g.ox * s), oy = (int)std::ceil(g.oy * s);
    const int w = ox + (int)std::ceil(((float)g.w - g.ox) * s), h = oy + (int)std::ceil(((float)g.h - g.oy) * s);
    if (w < 1 || h < 1 || w > 1000 || h > 500) return false;
    int px = 0, py = 0;
    if (!pack.Take(w, h, &px, &py)) return false;
    const uint32_t* src = f.px + (size_t)g.y * ArenaPics::kFontW + g.x;
    const float inv = 1.0f / s;
    const auto at = [&](int sx, int sy) -> uint32_t { return sx < 0 || sy < 0 || sx >= g.w || sy >= g.h ? 0u : src[(size_t)sy * ArenaPics::kFontW + sx]; };
    for (int j = 0; j < h; ++j) {
        const float ya = g.oy + (float)(j - oy) * inv, yb = ya + inv;
        uint32_t* row = atlas + (size_t)(py + j) * kAtlasW + px;
        for (int i = 0; i < w; ++i) {
            const float xa = g.ox + (float)(i - ox) * inv, xb = xa + inv;
            float sum[4] = {};
            if (s < 1.0f) {
                const int sy0 = std::max(0, (int)std::floor(ya)), sy1 = std::min((int)g.h, (int)std::ceil(yb));
                const int sx0 = std::max(0, (int)std::floor(xa)), sx1 = std::min((int)g.w, (int)std::ceil(xb));
                for (int sy = sy0; sy < sy1; ++sy) {
                    const float wy = std::min(yb, (float)(sy + 1)) - std::max(ya, (float)sy);
                    if (wy <= 0) continue;
                    for (int sx = sx0; sx < sx1; ++sx) {
                        const float wgt = wy * (std::min(xb, (float)(sx + 1)) - std::max(xa, (float)sx));
                        if (wgt <= 0) continue;
                        const uint32_t c = src[(size_t)sy * ArenaPics::kFontW + sx];
                        sum[0] += wgt * (float)(c >> 24), sum[1] += wgt * (float)((c >> 16) & 255), sum[2] += wgt * (float)((c >> 8) & 255), sum[3] += wgt * (float)(c & 255);
                    }
                }
                for (float& v : sum) v *= s * s;  // by the area of the pixel's place in the sheet
            } else {
                const float cx = (xa + xb) * 0.5f - 0.5f, cy = (ya + yb) * 0.5f - 0.5f;
                const int sx = (int)std::floor(cx), sy = (int)std::floor(cy);
                const float fx = cx - (float)sx, fy = cy - (float)sy;
                const uint32_t c4[4] = {at(sx, sy), at(sx + 1, sy), at(sx, sy + 1), at(sx + 1, sy + 1)};
                const float w4[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                for (int k = 0; k < 4; ++k)
                    sum[0] += w4[k] * (float)(c4[k] >> 24), sum[1] += w4[k] * (float)((c4[k] >> 16) & 255), sum[2] += w4[k] * (float)((c4[k] >> 8) & 255),
                        sum[3] += w4[k] * (float)(c4[k] & 255);
            }
            uint32_t ch[4];
            for (int k = 0; k < 4; ++k) ch[k] = (uint32_t)std::clamp((int)(sum[k] + 0.5f), 0, 255);
            for (int k = 1; k < 4; ++k) ch[k] = std::min(ch[k], ch[0]);  // a colour never brighter than its alpha allows
            row[i] = ch[0] << 24 | ch[1] << 16 | ch[2] << 8 | ch[3];
        }
    }
    out->x = (uint16_t)px, out->y = (uint16_t)py, out->w = (uint16_t)w, out->h = (uint16_t)h;
    out->dx = (int16_t)-ox, out->dy = (int16_t)-oy;
    return true;
}

bool BuildFaces(const ArenaPics::Art* art, const Hud::View& v) {
    const Hud::Frame fr = Hud::FrameOf(v);
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    memset(art->atlas + (size_t)(kAtlasH / 2) * kAtlasW, 0, (size_t)(kAtlasH / 2) * kAtlasW * 4);
    Packer pack;
    int letters = 0;
    for (int f = 0; f < kFaces; ++f) {
        const Spec& spec = kSpec[f];
        const ArenaPics::Font& font = art->font[spec.font];
        FaceData& face = gFace[f];
        face = FaceData{};
        if (!font.px || !(font.cap > 1.0f) || !(font.em > 1.0f)) return false;
        face.cap = std::max(7, (int)std::lround(spec.capUnits * fr.u));
        const float s = (float)face.cap / font.cap;
        face.kern = font.kern;
        face.kernPx = font.em * s / 20480.0f;
        for (int k = 0; k < kCodes; ++k) {
            const ArenaPics::Glyph& g = font.glyph[k];
            face.letter[k].advance = g.advance * font.em * s;
            if (!g.w || (spec.only && !strchr(spec.only, kFirstCode + k))) continue;
            if (!ScaleLetter(font, g, s, art->atlas, pack, &face.letter[k])) {
                snprintf(gText, sizeof gText, "the lettering for %dx%d does not fit its texture (face %d, capitals %d px)", v.w, v.h, f, face.cap);
                return false;
            }
            ++letters;
        }
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    snprintf(gText, sizeof gText, "MK9 lettering for %dx%d: capitals %d, %d and %d px, %d letters in %d of %d rows of the texture, %.1f ms", v.w, v.h, gFace[kTitle].cap,
             gFace[kLine].cap, gFace[kName].cap, letters, pack.y + pack.rowH - kAtlasH / 2, kAtlasH / 2,
             (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)(fq.QuadPart ? fq.QuadPart : 1));
    return true;
}

int CodeOf(char ch) {
    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < kFirstCode || c >= kFirstCode + kCodes) c = '?';
    return c - kFirstCode;
}

void Put(Canvas& c, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br) {
    if (x1 <= x0 || y1 <= y0) return;
    if (c.n + 6 > c.cap) {
        ++c.dropped;
        return;
    }
    x0 -= 0.5f, y0 -= 0.5f, x1 -= 0.5f, y1 -= 0.5f;
    const Vtx a{x0, y0, 0, 1, tl, u0, v0}, b{x1, y0, 0, 1, tr, u1, v0}, d{x0, y1, 0, 1, bl, u0, v1}, e{x1, y1, 0, 1, br, u1, v1};
    Vtx* v = c.v + c.n;
    v[0] = a, v[1] = b, v[2] = d, v[3] = b, v[4] = e, v[5] = d;
    c.n += 6;
}
}  // namespace

namespace StageSkin {

bool Prepare(const ArenaPics::Art* art, const Hud::View& v) {
    if (!art || !art->ok || !art->atlas) {
        gReady = false;
        return false;
    }
    if (gReady && gArt == art && gForW == v.w && gForH == v.h) return true;
    if (gFailArt == art && gFailW == v.w && gFailH == v.h) return false;  // it did not fit: not tried again every frame
    gReady = BuildFaces(art, v);
    ++gVersion;
    gArt = art, gForW = v.w, gForH = v.h;
    if (gReady) gFailArt = nullptr;
    else gFailArt = art, gFailW = v.w, gFailH = v.h;
    return gReady;
}

bool ReadyFor(const Hud::View& v) { return gReady && gForW == v.w && gForH == v.h; }
uint32_t Version() { return gVersion; }
void Describe(char* out, int n) {
    if (n > 0) snprintf(out, (size_t)n, "%s", gText);
}
void Reset() {
    gReady = false;
    gArt = gFailArt = nullptr;
    gForW = gForH = gFailW = gFailH = 0;
}

uint32_t Pm(uint32_t rgb, int alpha) {
    const uint32_t a = (uint32_t)std::clamp(alpha, 0, 255);
    return a << 24 | (((rgb >> 16) & 255) * a + 127) / 255 << 16 | (((rgb >> 8) & 255) * a + 127) / 255 << 8 | ((rgb & 255) * a + 127) / 255;
}
uint32_t Lit(uint32_t rgb, int strength) { return Pm(rgb, strength) & 0x00FFFFFFu; }

void Quad(Canvas& c, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br) {
    Put(c, (float)x0, (float)y0, (float)x1, (float)y1, u0, v0, u1, v1, tl, tr, bl, br);
}
void Fill4(Canvas& c, int x0, int y0, int x1, int y1, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br) {
    const ArenaPics::Place w = ArenaPics::PlaceOf(ArenaPics::kWhite);  // every corner at the white block's middle
    const float u = ((float)w.x + (float)w.w * 0.5f) / (float)kAtlasW, v = ((float)w.y + (float)w.h * 0.5f) / (float)kAtlasH;
    Put(c, (float)x0, (float)y0, (float)x1, (float)y1, u, v, u, v, tl, tr, bl, br);
}
void FillV(Canvas& c, int x0, int y0, int x1, int y1, uint32_t top, uint32_t bottom) { Fill4(c, x0, y0, x1, y1, top, top, bottom, bottom); }
void FillH(Canvas& c, int x0, int y0, int x1, int y1, uint32_t left, uint32_t right) { Fill4(c, x0, y0, x1, y1, left, right, left, right); }
void Fill(Canvas& c, int x0, int y0, int x1, int y1, uint32_t colour) { FillV(c, x0, y0, x1, y1, colour, colour); }

void PiecePart(Canvas& c, ArenaPics::PieceId p, int sx0, int sx1, int x0, int y0, int x1, int y1, uint32_t colour) {
    const ArenaPics::Place pl = ArenaPics::PlaceOf(p);
    if (!pl.w || sx0 == sx1) return;
    // Half a pixel inside the part's outer edges; a mirrored part (sx0 > sx1) the same, the other way round.
    const float in = sx0 < sx1 ? 0.5f : -0.5f;
    const float u0 = ((float)(pl.x + sx0) + in) / (float)kAtlasW, u1 = ((float)(pl.x + sx1) - in) / (float)kAtlasW;
    const float v0 = ((float)pl.y + 0.5f) / (float)kAtlasH, v1 = ((float)(pl.y + pl.h) - 0.5f) / (float)kAtlasH;
    Put(c, (float)x0, (float)y0, (float)x1, (float)y1, u0, v0, u1, v1, colour, colour, colour, colour);
}
void Glow(Canvas& c, ArenaPics::PieceId p, int x0, int y0, int x1, int y1, int reach, uint32_t colour) {
    const ArenaPics::Place pl = ArenaPics::PlaceOf(p);
    if (!pl.w || reach < 1 || x1 - x0 < 2 || y1 - y0 < 2) return;
    // The piece's four quarters at the corners, its middle column and row stretched along the edges; each reaches as
    // far inside the box as outside (the inside is covered by what the box holds).
    const float ul = ((float)pl.x + 0.5f) / (float)kAtlasW, um = ((float)pl.x + (float)pl.w * 0.5f) / (float)kAtlasW, ur = ((float)(pl.x + pl.w) - 0.5f) / (float)kAtlasW;
    const float vt = ((float)pl.y + 0.5f) / (float)kAtlasH, vm = ((float)pl.y + (float)pl.h * 0.5f) / (float)kAtlasH, vb = ((float)(pl.y + pl.h) - 0.5f) / (float)kAtlasH;
    const int r = std::min({2 * reach, (x1 - x0) / 2 + reach, (y1 - y0) / 2 + reach});
    const float xs[4] = {(float)(x0 - reach), (float)(x0 - reach + r), (float)(x1 + reach - r), (float)(x1 + reach)}, us[4] = {ul, um, um, ur};
    const float ys[4] = {(float)(y0 - reach), (float)(y0 - reach + r), (float)(y1 + reach - r), (float)(y1 + reach)}, vs[4] = {vt, vm, vm, vb};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            if (i != 1 || j != 1) Put(c, xs[i], ys[j], xs[i + 1], ys[j + 1], us[i], vs[j], us[i + 1], vs[j + 1], colour, colour, colour, colour);
}
void Piece(Canvas& c, ArenaPics::PieceId p, int x0, int y0, int x1, int y1, uint32_t colour, bool mirrored) {
    const int w = ArenaPics::PlaceOf(p).w;
    PiecePart(c, p, mirrored ? w : 0, mirrored ? 0 : w, x0, y0, x1, y1, colour);
}
void Picture(Canvas& c, int slot, int x0, int y0, int x1, int y1, uint32_t colour) {
    if (slot < 0 || slot >= ArenaPics::kSlots) return;
    const float px = (float)(slot % 8 * ArenaPics::kSide), py = (float)(slot / 8 * ArenaPics::kSide);
    Put(c, (float)x0, (float)y0, (float)x1, (float)y1, (px + 0.5f) / (float)kAtlasW, (py + 0.5f) / (float)kAtlasH, (px + (float)ArenaPics::kSide - 0.5f) / (float)kAtlasW,
        (py + (float)ArenaPics::kSide - 0.5f) / (float)kAtlasH, colour, colour, colour, colour);
}

int CapOf(Face f) { return f >= 0 && f < kFaces ? gFace[f].cap : 0; }

int Width(Face f, const char* s) {
    if (f < 0 || f >= kFaces || !s || !gFace[f].kern) return 0;
    const FaceData& face = gFace[f];
    float pen = 0;
    int prev = -1;
    for (; *s; ++s) {
        const int k = CodeOf(*s);
        if (prev >= 0) pen += (float)face.kern[prev][k] * face.kernPx;
        pen += face.letter[k].advance;
        prev = k;
    }
    return (int)std::ceil(pen);
}

int Write(Canvas& c, Face f, const char* s, int x, int y, uint32_t colour) {
    if (f < 0 || f >= kFaces || !s || !gFace[f].kern) return 0;
    const FaceData& face = gFace[f];
    // A large face puts every letter on a whole pixel (sharp). In a small one half a pixel is a tenth of a letter's
    // width and whole pixels make the gaps between letters visibly unequal: there a letter stands where its pen is,
    // between two pixels if need be, which the display's filter draws a little softer and evenly spaced.
    const bool fine = face.cap < kFineBelow;
    float pen = 0;
    int prev = -1;
    for (; *s; ++s) {
        const int k = CodeOf(*s);
        if (prev >= 0) pen += (float)face.kern[prev][k] * face.kernPx;
        const Letter& l = face.letter[k];
        if (l.w) {
            const float qx = (float)(x + l.dx) + (fine ? pen : std::round(pen)), qy = (float)(y + l.dy);
            Put(c, qx, qy, qx + (float)l.w, qy + (float)l.h, (float)l.x / (float)kAtlasW, (float)l.y / (float)kAtlasH, (float)(l.x + l.w) / (float)kAtlasW,
                (float)(l.y + l.h) / (float)kAtlasH, colour, colour, colour, colour);
        }
        pen += l.advance;
        prev = k;
    }
    return (int)std::ceil(pen);
}

}  // namespace StageSkin
