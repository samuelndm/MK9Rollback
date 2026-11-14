// Host test of src\debug\StageScreen.cpp: the stage screen of an online match without the game. For each picture size
// and each canned state it builds the frame's rectangles, checks them (inside the picture and the panel, none dropped,
// names that fit their cells), times the build and draws pictures into build\hosttests\stagescreen\*.bmp; then the
// grid's navigation, the roulette's timing, the stage names and what a damaged state does.
// Both forms of the grid: the list of names (stage_*.bmp) and the picture tiles (tiles_*.bmp). The tiles' pictures
// are drawn by this test (coloured patterns), or read from the installed game's arena package when its path is
// given: those output pictures then show the game's art and stay in build\ (not tracked, not released).
//   stagescreen_test.exe <out folder> [background.bmp or -] [the game's ui_ps_arenaSelect.xxx]
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../src/debug/ArenaPics.h"
#include "../../src/debug/StageScreen.h"
#include "../../src/debug/StageSkin.h"

namespace {
int gChecks = 0, gFailed = 0;
void Check(bool ok, const char* what, const char* detail = "") {
    ++gChecks;
    if (!ok) {
        ++gFailed;
        printf("FAIL %s %s\n", what, detail);
    }
}

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // BGR, top-down
};

bool LoadBmp(const char* path, Image& img) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    bool ok = fread(&fh, sizeof fh, 1, f) == 1 && fread(&ih, sizeof ih, 1, f) == 1 && fh.bfType == 0x4D42 &&
              (ih.biBitCount == 24 || ih.biBitCount == 32) && ih.biCompression == BI_RGB && ih.biWidth > 0;
    if (ok) {
        const int h = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight, bytes = ih.biBitCount / 8;
        const int pitch = (ih.biWidth * bytes + 3) & ~3;
        std::vector<uint8_t> row((size_t)pitch);
        img.w = ih.biWidth;
        img.h = h;
        img.px.resize((size_t)img.w * h * 3);
        fseek(f, (long)fh.bfOffBits, SEEK_SET);
        for (int y = 0; y < h && ok; ++y) {
            ok = fread(row.data(), 1, (size_t)pitch, f) == (size_t)pitch;
            const int dy = ih.biHeight < 0 ? y : h - 1 - y;
            for (int x = 0; x < img.w; ++x) memcpy(&img.px[((size_t)dy * img.w + x) * 3], &row[(size_t)x * bytes], 3);
        }
    }
    fclose(f);
    return ok;
}

bool SaveBmp(const char* path, const Image& img) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const int pitch = (img.w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)pitch * img.h;
    ih.biSize = sizeof ih;
    ih.biWidth = img.w;
    ih.biHeight = -img.h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    std::vector<uint8_t> row((size_t)pitch, 0);
    for (int y = 0; y < img.h; ++y) {
        memcpy(row.data(), &img.px[(size_t)y * img.w * 3], (size_t)img.w * 3);
        fwrite(row.data(), 1, (size_t)pitch, f);
    }
    fclose(f);
    return true;
}

// The picture the game would have drawn under the panel: black, with the 16:9 frame holding the background (a
// screenshot when one is given, else bright and dark blocks, so that text is judged over both).
Image Backdrop(int w, int h, const Image* shot) {
    Image img;
    img.w = w, img.h = h;
    img.px.assign((size_t)w * h * 3, 0);
    int fw = w, fh = w * 9 / 16;
    if ((long long)w * 9 >= (long long)h * 16) fh = h, fw = h * 16 / 9;
    const int fx = (w - fw) / 2, fy = (h - fh) / 2;
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) {
            uint8_t* p = &img.px[((size_t)(fy + y) * w + fx + x) * 3];
            if (shot && shot->w) {
                const int sx = (int)((long long)x * shot->w / fw), sy = (int)((long long)y * shot->h / fh);
                memcpy(p, &shot->px[((size_t)sy * shot->w + sx) * 3], 3);
            } else {
                const int bx = x * 8 / fw, by = y * 6 / fh;
                const int v = ((bx + by) & 1) ? 235 : 40 + x * 120 / fw;
                p[0] = (uint8_t)(v * 8 / 10), p[1] = (uint8_t)(v * 9 / 10), p[2] = (uint8_t)v;
            }
        }
    return img;
}

void Raster(Image& img, const Hud::Vtx* v, int n) {
    for (int i = 0; i + 6 <= n; i += 6) {
        const int x0 = (int)(v[i].x + 0.5f), y0 = (int)(v[i].y + 0.5f), x1 = (int)(v[i + 4].x + 0.5f), y1 = (int)(v[i + 4].y + 0.5f);
        const uint32_t c = v[i].color;
        const int a = (int)(c >> 24), r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < img.h; ++y)
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < img.w; ++x) {
                uint8_t* p = &img.px[((size_t)y * img.w + x) * 3];
                p[0] = (uint8_t)((b * a + p[0] * (255 - a)) / 255);
                p[1] = (uint8_t)((g * a + p[1] * (255 - a)) / 255);
                p[2] = (uint8_t)((r * a + p[2] * (255 - a)) / 255);
            }
    }
}

// The tiles' pictures, as Overlay.cpp draws them from the atlas (here: the nearest pixel).
void RasterPics(Image& img, const StageScreen::Pic* pics, int n, const uint32_t* atlas) {
    for (int k = 0; k < n; ++k) {
        const StageScreen::Pic& p = pics[k];
        if (p.slot >= ArenaPics::kSlots || p.x1 <= p.x0 || p.y1 <= p.y0) continue;
        const int ox = p.slot % 8 * ArenaPics::kSide, oy = p.slot / 8 * ArenaPics::kSide;
        for (int y = p.y0 < 0 ? 0 : p.y0; y < p.y1 && y < img.h; ++y)
            for (int x = p.x0 < 0 ? 0 : p.x0; x < p.x1 && x < img.w; ++x) {
                const int sx = ox + (x - p.x0) * ArenaPics::kSide / (p.x1 - p.x0), sy = oy + (y - p.y0) * ArenaPics::kSide / (p.y1 - p.y0);
                const uint32_t c = atlas[(size_t)sy * ArenaPics::kAtlasW + sx];
                uint8_t* q = &img.px[((size_t)y * img.w + x) * 3];
                q[0] = (uint8_t)c, q[1] = (uint8_t)(c >> 8), q[2] = (uint8_t)(c >> 16);
            }
    }
}

// MK9's look, as Overlay.cpp has the display draw it: every rectangle filled from the atlas between its four nearest
// pixels, times the colour that runs between the rectangle's corners, and laid over the picture (the colours are
// already multiplied by alpha: what is under it keeps 1 - alpha).
void RasterLook(Image& img, const StageSkin::Vtx* v, int n, const uint32_t* atlas) {
    const int W = ArenaPics::kAtlasW, H = ArenaPics::kAtlasH;
    for (int i = 0; i + 6 <= n; i += 6) {
        // A rectangle may begin between two pixels (small lettering): a pixel is drawn when its middle lies inside.
        const float left = v[i].x + 0.5f, right = v[i + 4].x + 0.5f;
        const int x0 = (int)std::ceil(left - 0.5f), x1 = (int)std::ceil(right - 0.5f), y0 = (int)(v[i].y + 0.5f), y1 = (int)(v[i + 4].y + 0.5f);
        if (right <= left || y1 <= y0) continue;
        const uint32_t corner[4] = {v[i].color, v[i + 1].color, v[i + 2].color, v[i + 4].color};  // top left, top right, bottom left, bottom right
        const float u0 = v[i].u * (float)W, v0 = v[i].v * (float)H, u1 = v[i + 4].u * (float)W, v1 = v[i + 4].v * (float)H;
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < img.h; ++y) {
            const float ty = ((float)(y - y0) + 0.5f) / (float)(y1 - y0), sv = v0 + ty * (v1 - v0) - 0.5f;
            const int iy = (int)std::floor(sv);
            const float fy = sv - (float)iy;
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < img.w; ++x) {
                const float tx = ((float)x + 0.5f - left) / (right - left), su = u0 + tx * (u1 - u0) - 0.5f;
                const int ix = (int)std::floor(su);
                const float fx = su - (float)ix;
                float tex[4] = {}, col[4];
                for (int k = 0; k < 4; ++k) {
                    const int px = std::clamp(ix + (k & 1), 0, W - 1), py = std::clamp(iy + (k >> 1), 0, H - 1);
                    const uint32_t c = atlas[(size_t)py * W + px];
                    const float w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                    for (int ch = 0; ch < 4; ++ch) tex[ch] += w * (float)((c >> (8 * ch)) & 255);
                }
                for (int ch = 0; ch < 4; ++ch) {
                    const float top = (float)((corner[0] >> (8 * ch)) & 255) * (1 - tx) + (float)((corner[1] >> (8 * ch)) & 255) * tx;
                    const float bottom = (float)((corner[2] >> (8 * ch)) & 255) * (1 - tx) + (float)((corner[3] >> (8 * ch)) & 255) * tx;
                    col[ch] = tex[ch] * (top * (1 - ty) + bottom * ty) / 255.0f;
                }
                uint8_t* q = &img.px[((size_t)y * img.w + x) * 3];
                for (int ch = 0; ch < 3; ++ch) q[ch] = (uint8_t)std::clamp((int)(col[ch] + (float)q[ch] * (1.0f - col[3] / 255.0f) + 0.5f), 0, 255);
            }
        }
    }
}

// Art of this test's own, for a PC without the game: two fonts whose every sign is a bar with a notch, in a sheet laid
// out like the game's (boxes with a margin, the pen near the box's lower left), and frame pieces that are plain shapes.
void MakeArt(ArenaPics::Art& art) {
    using namespace ArenaPics;
    for (int f = 0; f < kFonts; ++f) {
        Font& font = art.font[f];
        memset(font.px, 0, (size_t)kFontW * kFontH * 4);
        font.em = 87.5f, font.cap = 64.0f, font.glyphs = font.pairs = 0;
        memset(font.kern, 0, sizeof font.kern);
        for (int k = 0; k < kCodes; ++k) {
            Glyph& g = font.glyph[k];
            g = Glyph{};
            g.advance = k == 0 ? 0.25f : 0.36f;
            if (k == 0) continue;  // the space
            g.x = (uint16_t)(k % 16 * 64 + 4), g.y = (uint16_t)(k / 16 * 128 + 4), g.w = 40, g.h = 80;
            g.ox = 6.0f, g.oy = 70.0f;
            for (int y = 6; y < 70; ++y)
                for (int x = 8; x < 32; ++x) {
                    const bool edge = y < 10 || y >= 66 || x < 12 || x >= 28, notch = !edge && (y - 10) / 14 == k % 4 && x >= 20;
                    if (notch) continue;
                    font.px[(size_t)(g.y + y) * kFontW + g.x + x] = edge ? (f ? 0xFF7A1010u : 0xFFC9A24Eu) : (f ? 0xFFFFF1C8u : 0xFFE24A2Au);
                }
            ++font.glyphs;
        }
        font.kern['A' - kFirstCode]['V' - kFirstCode] = -1600;
        font.pairs = 1;
    }
    for (int y = 0; y < kAtlasH / 2; ++y) memset(art.atlas + (size_t)y * kAtlasW + kAtlasW / 2, 0, (size_t)(kAtlasW / 2) * 4);
    const auto put = [&](PieceId id, int x, int y, uint32_t rgb, int alpha) { art.atlas[(size_t)(PlaceOf(id).y + y) * kAtlasW + PlaceOf(id).x + x] = StageSkin::Pm(rgb, alpha); };
    for (int id = 0; id < kPieces; ++id) {
        const Place p = PlaceOf((PieceId)id);
        for (int y = 0; y < p.h; ++y)
            for (int x = 0; x < p.w; ++x) {
                const float dx = ((float)x + 0.5f) / (float)p.w - 0.5f, dy = ((float)y + 0.5f) / (float)p.h - 0.5f, r = std::sqrt(dx * dx + dy * dy) * 2.0f;
                switch (id) {
                case kBar: put(kBar, x, y, 0xC9A24E, (y >= 38 && y < 43) || ((x < 60 || x >= 452) && y >= 4 && y < 60 && ((x / 6 + y / 6) & 1)) ? 255 : 0); break;
                case kSun: put(kSun, x, y, 0xC9A24E, (y >= 4 && y < 7) ? 255 - std::abs(x - 256) : (std::abs(x - 256) < 30 - y / 2 && y >= 7) ? 255 : 0); break;
                case kGlow: put(kGlow, x, y, 0xFF9A3C, (int)(255.0f * ((float)x / (float)p.w) * std::max(0.0f, 1.0f - std::abs(dy) * 2.2f))); break;
                case kHalo: put(kHalo, x, y, 0xFFFFFF, (int)(255.0f * std::clamp((1.0f - r) * 3.0f, 0.0f, 1.0f))); break;
                case kRing: put(kRing, x, y, 0xC9A24E, r > 0.86f && r < 0.96f ? 255 : 0); break;
                case kRed: put(kRed, x, y, 0x7A1004, (int)(255.0f * std::clamp(1.0f - r, 0.0f, 1.0f))); break;
                default: put(kWhite, x, y, 0xFFFFFF, 255); break;
                }
            }
    }
    art.ok = true;
}

constexpr int kCap = 360000;  // Overlay.cpp's buffer for the panel
Hud::Vtx gV[kCap];

double NowUs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}

struct Named {
    const char* table;
    const char* shown;
};
const Named kNames[] = {
    {"BGND_ChallengeTower", "CHALLENGE TOWER"}, {"BGND_Courtyard", "COURTYARD"}, {"BGND_CourtyardN", "COURTYARD NIGHT"},
    {"BGND_DeadPool", "DEAD POOL"}, {"BGND_Desert", "DESERT"}, {"BGND_FleshPits", "FLESH PITS"}, {"BGND_GorosLair", "GORO'S LAIR"},
    {"BGND_Graveyard", "GRAVEYARD"}, {"BGND_Hell", "HELL"}, {"BGND_KahnsColiseum", "KAHN'S COLISEUM"},
    {"BGND_LivingForest", "LIVING FOREST"}, {"BGND_Pit", "PIT"}, {"BGND_PitBottom", "PIT BOTTOM"}, {"BGND_PitDay", "PIT DAY"},
    {"BGND_RooftopDawn", "ROOFTOP DAWN"}, {"BGND_RooftopDay", "ROOFTOP DAY"}, {"BGND_RooftopDusk", "ROOFTOP DUSK"},
    {"BGND_SKThrone", "SHAO KAHN THRONE"}, {"BGND_SoulChamber", "SOUL CHAMBER"}, {"BGND_STGardens", "SHANG TSUNG GARDENS"},
    {"BGND_STGardensN", "SHANG TSUNG GARDENS NIGHT"}, {"BGND_Street", "STREET"}, {"BGND_STThrone", "SHANG TSUNG THRONE"},
    {"BGND_Subway", "SUBWAY"}, {"BGND_Temple", "TEMPLE"}, {"BGND_Tower", "TOWER"}, {"BGND_Wastelands", "WASTELANDS"},
    {"BGND_Armory", "ARMORY"}, {"BGND_BellTower", "BELL TOWER"}, {"BGND_BioViewer", "BIO VIEWER"}, {"BGND_KOTH", "KOTH"},
    {"BGND_KOTH_S", "KOTH S"}, {"BGND_Krypt", "KRYPT"}, {"BGND_Nekropolis", "NEKROPOLIS"},
    {"bgnd_pit", "PIT"}, {"N", "N"}, {"", "?"}, {"BGND_", "?"}, {"Pit_2", "PIT 2"}, {"SKY", "SKY"}, {"STAGE", "STAGE"}};

bool Inside(const StageScreen::Box& in, const StageScreen::Box& out) {
    return in.x0 >= out.x0 && in.y0 >= out.y0 && in.x1 <= out.x1 && in.y1 <= out.y1 && in.x0 < in.x1 && in.y0 < in.y1;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace StageScreen;
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* outDir = argc > 1 ? argv[1] : ".";
    Image shot;
    if (argc > 2 && strcmp(argv[2], "-") && !LoadBmp(argv[2], shot)) printf("background %s not readable; using the blocks\n", argv[2]);
    CreateDirectoryA(outDir, nullptr);

    // The tiles' pictures: the installed game's when its package is given and readable, else patterns of this test's
    // own under the 29 names the game's package has.
    // The same for MK9's look: the installed game's lettering and frame pieces, else shapes of this test's own. All of
    // it stands in one atlas, as in the mod: the pictures in its upper left quarter.
    static std::vector<uint32_t> atlasPx((size_t)ArenaPics::kAtlasW * ArenaPics::kAtlasH), lettersPx((size_t)ArenaPics::kFonts * ArenaPics::kFontW * ArenaPics::kFontH);
    uint32_t* const sheet = atlasPx.data();
    static ArenaPics::Set pics;
    static ArenaPics::Art art;
    pics.sheet = sheet, pics.stride = ArenaPics::kAtlasW;
    art.atlas = sheet;
    for (int i = 0; i < ArenaPics::kFonts; ++i) art.font[i].px = lettersPx.data() + (size_t)i * ArenaPics::kFontW * ArenaPics::kFontH;
    bool gamePictures = false;
    if (argc > 3 && argv[3][0]) {
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[3], -1, wide, MAX_PATH);
        gamePictures = ArenaPics::LoadFile(wide, &pics, &art);
        if (!gamePictures) printf("the game's arena package was not read (%s); the tiles show this test's patterns\n", pics.why);
    }
    if (!gamePictures) {
        pics = ArenaPics::Set{};
        pics.sheet = sheet, pics.stride = ArenaPics::kAtlasW;
        for (int i = 0; i < 29; ++i) {  // kNames[0..28]: the challenge tower and the 28 arenas a player may pick
            ArenaPics::PictureName(kNames[i].table, pics.name[i], sizeof pics.name[i]);
            const int ox = i % 8 * ArenaPics::kSide, oy = i / 8 * ArenaPics::kSide;
            for (int y = 0; y < ArenaPics::kSide; ++y)
                for (int x = 0; x < ArenaPics::kSide; ++x) {
                    const int band = ((x + y) / 16 + i) % 3, r = 40 + i * 7 + (band == 0 ? 70 : 0), g = 60 + y + (band == 1 ? 50 : 0), b = 90 + x / 2 + (band == 2 ? 60 : 0);
                    sheet[(size_t)(oy + y) * ArenaPics::kAtlasW + ox + x] = 0xFF000000u | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
                }
        }
        pics.count = 29;
    }
    const bool gameArt = gamePictures && art.ok;
    if (gamePictures && !art.ok) printf("the game's lettering was not read (%s); MK9's look is drawn with this test's shapes\n", art.why);
    if (!gameArt) MakeArt(art);
    printf("the tiles' pictures: %s (%d); the look's lettering and pieces: %s\n", gamePictures ? "the installed game's" : "this test's patterns", pics.count,
           gameArt ? "the installed game's" : "this test's shapes");
    static const int kSizes[][2] = {{1920, 1080}, {1920, 1200}, {1280, 720}, {1024, 768}, {960, 600}, {2560, 1440}, {3840, 2160}, {3440, 1440}};
    char text[256], path[MAX_PATH];
    int worstVerts = 0, pictures = 0, worstLook = 0;
    double worstBuildUs = 0, worstFacesUs = 0;

    for (const auto& size : kSizes) {
        const Hud::View view{0, 0, size[0], size[1]};
        Hud::Reset();
        snprintf(text, sizeof text, "%dx%d", size[0], size[1]);
        Check(Hud::Prepare(view) && Hud::FacesFor(view), "faces built", text);
        if (!Hud::FacesFor(view)) continue;
        const Hud::Frame fr = Hud::FrameOf(view);
        const bool everyPicture = (size[0] == 1920 && size[1] == 1080) || size[0] == 1280 || size[0] == 960;
        // Every canned state three times: as the list of names (no pictures read), as plain picture tiles (pictures,
        // no lettering), and in MK9's look.
        for (int form = 0; form < 3; ++form)
        for (int demo = 1; demo <= kDemos; ++demo) {
            ArenaPics::Adopt(form ? &pics : nullptr, form == 2 ? &art : nullptr);
            StageSkin::Reset();
            if (form == 2) {
                const double tb = NowUs();
                const bool built = StageSkin::Prepare(ArenaPics::ArtOf(), view);
                const double us = NowUs() - tb;
                snprintf(text, sizeof text, "%dx%d", size[0], size[1]);
                Check(built && StageSkin::ReadyFor(view), "MK9's look: the lettering fits its texture at this size", text);
                if (demo == 1) {
                    char line[220];
                    StageSkin::Describe(line, sizeof line);
                    printf("%s (this build %.1f ms)\n", line, us / 1000.0);
                    if (us > worstFacesUs) worstFacesUs = us;
                }
            }
            State s;
            const uint64_t now = 5000000ull + 1000ull * (uint64_t)demo;
            Check(Demo(demo, now, &s), "canned state exists");
            const int n = BuildState(s, view, now, gV, kCap);
            const StageSkin::Vtx* lookV = nullptr;
            const int lookN = Look(&lookV);
            snprintf(text, sizeof text, "%dx%d %s state %d: %d vertices", size[0], size[1], form == 2 ? "look" : form ? "tiles" : "names", demo, form == 2 ? lookN : n);
            const Pic* picList = nullptr;
            int split = 0;
            const int picN = Pictures(&picList, &split);
            const bool looked = form == 2 && s.phase != kBanner && s.cells <= kTileCells;
            Check((lookN > 0) == looked, "MK9's look is drawn where there are tiles and its art, and nowhere else", text);
            if (looked) {
                // The plain rectangles are the dimming alone. Every textured rectangle: whole, inside the panel, filled
                // from inside the atlas; a letter is drawn pixel for pixel; 28 pictures, each exactly in its tile's place.
                const Layout l = LayoutOf(s.cells, true, view);
                bool whole = n == 6 && lookN % 6 == 0 && Dropped() == 0, onPanel = true, inAtlas = true, sharp = true, placed = true;
                int letters = 0, shown = 0;
                for (int i = 0; i + 6 <= lookN; i += 6) {
                    const StageSkin::Vtx& a = lookV[i];
                    const StageSkin::Vtx& e = lookV[i + 4];
                    onPanel = onPanel && a.x >= (float)l.panel.x0 - 0.5f && e.x <= (float)l.panel.x1 && a.y >= (float)l.panel.y0 - 0.5f && e.y <= (float)l.panel.y1 && a.x < e.x && a.y < e.y;
                    for (int k = 0; k < 6; ++k) inAtlas = inAtlas && lookV[i + k].u >= 0.0f && lookV[i + k].u <= 1.0f && lookV[i + k].v >= 0.0f && lookV[i + k].v <= 1.0f;
                    const float tw = std::abs(e.u - a.u) * (float)ArenaPics::kAtlasW, th = (e.v - a.v) * (float)ArenaPics::kAtlasH;
                    if (a.v >= 0.5f) {  // the lower half: lettering
                        ++letters;
                        sharp = sharp && std::abs(tw - (e.x - a.x)) < 0.01f && std::abs(th - (e.y - a.y)) < 0.01f && e.v <= 1.0f;
                    } else if (a.u < 0.5f && e.u <= 0.5f && tw > 100.0f) {  // the upper left quarter: an arena picture
                        placed = placed && shown < s.cells && (int)(a.x + 0.5f) == l.pic[shown].x0 && (int)(a.y + 0.5f) == l.pic[shown].y0 &&
                                 (int)(e.x + 0.5f) == l.pic[shown].x1 && (int)(e.y + 0.5f) == l.pic[shown].y1 && s.pic[shown] != 0 &&
                                 (int)(a.u * (float)ArenaPics::kAtlasW) / ArenaPics::kSide + 8 * ((int)(a.v * (float)ArenaPics::kAtlasH) / ArenaPics::kSide) == s.pic[shown] - 1;
                        ++shown;
                    }
                }
                snprintf(text + strlen(text), sizeof text - strlen(text), ", %d letters, %d pictures", letters, shown);
                if (demo == 1 || demo == 4) puts(text);
                Check(whole && picN == 0, "look: the plain rectangles are the dimming alone, the rest whole textured rectangles, none dropped", text);
                Check(onPanel, "look: every rectangle stays on the panel", text);
                Check(inAtlas, "look: every rectangle is filled from inside the atlas", text);
                Check(sharp && letters > 100, "look: every letter is drawn pixel for pixel", text);
                Check(placed && shown == s.cells - 1, "look: every arena's picture exactly in its tile's place", text);
                Check(gV[0].x <= 0.0f && gV[4].x >= (float)size[0] - 0.5f && gV[4].y >= (float)size[1] - 0.5f, "look: the picture behind the panel is dimmed", text);
                if (lookN > worstLook) worstLook = lookN;
                const double t0 = NowUs();
                for (int k = 0; k < 40; ++k) BuildState(s, view, now, gV, kCap);
                const double us = (NowUs() - t0) / 40.0;
                if (us > worstBuildUs) worstBuildUs = us;
                if (everyPicture || demo == 1 || demo == 5) {
                    Image img = Backdrop(size[0], size[1], &shot);
                    Raster(img, gV, n);
                    RasterLook(img, lookV, lookN, sheet);
                    snprintf(path, sizeof path, "%s\\look_%dx%d_%02d.bmp", outDir, size[0], size[1], demo);
                    Check(SaveBmp(path, img), "picture written", path);
                    ++pictures;
                }
                continue;
            }
            const bool tiled = form && s.phase != kBanner && s.cells <= kTileCells;
            Check(s.tiles == (form ? 1 : 0) && Tiles(s.cells, s.tiles != 0) == (form && s.cells <= kTileCells), "the canned state takes the form the pictures allow", text);
            if (form == 2 && demo != 12 && s.phase != kBanner) continue;  // (reached only if the look was not drawn: reported above)
            Check(n > 0 && n % 6 == 0 && n <= kCap, "a frame of whole rectangles", text);
            Check(Dropped() == 0, "no rectangle dropped", text);
            if (n > worstVerts) worstVerts = n;
            if (demo == 1 || demo == 4 || demo == 12) puts(text);
            bool inside = true;
            for (int i = 0; i < n; ++i)
                inside = inside && gV[i].x >= -0.5f && gV[i].y >= -0.5f && gV[i].x <= (float)size[0] && gV[i].y <= (float)size[1];
            Check(inside, "every rectangle inside the picture", text);
            const Layout l = LayoutOf(s.cells, s.tiles != 0, view);
            if (tiled) {
                // 28 arenas have a picture, RANDOM has none; each picture lies in its tile, 16:9, and is drawn after
                // what lies under it and before what lies over it.
                bool ok = picN == s.cells - 1 && split > 0 && split < n && split % 6 == 0;
                for (int i = 0; i < picN && ok; ++i) {
                    const Box b{picList[i].x0, picList[i].y0, picList[i].x1, picList[i].y1};
                    ok = Inside(b, l.cell[i]) && Inside(b, l.panel) && picList[i].slot < pics.count && b.x0 == l.pic[i].x0 && b.y1 == l.pic[i].y1;
                    const int w = b.x1 - b.x0, h = b.y1 - b.y0;
                    ok = ok && h * 16 >= w * 9 - 16 && h * 16 <= w * 9 + 16;
                    if (ok && !gamePictures) ok = !strcmp(pics.name[picList[i].slot], pics.name[i + 1]);  // the test's own set: arena i is picture i + 1
                }
                snprintf(text + strlen(text), sizeof text - strlen(text), ", %d pictures, drawn before vertex %d", picN, split);
                Check(ok, "tiles: every arena's picture in its tile, 16:9, between the two layers", text);
            } else {
                Check(picN == 0 && split == 0, "no pictures where the list of names or the banner is shown", text);
            }
            if (s.phase != kBanner) {
                // The first rectangle dims the whole picture; everything after it stays on the panel.
                bool onPanel = true;
                for (int i = 6; i < n; ++i)
                    onPanel = onPanel && gV[i].x >= (float)l.panel.x0 - 0.5f && gV[i].x <= (float)l.panel.x1 && gV[i].y >= (float)l.panel.y0 - 0.5f &&
                              gV[i].y <= (float)l.panel.y1;
                Check(onPanel, "every text and mark stays on the panel", text);
                Check(gV[0].x <= 0.0f && gV[4].x >= (float)size[0] - 0.5f && gV[4].y >= (float)size[1] - 0.5f, "the picture behind the panel is dimmed", text);
            } else {
                bool inFrame = true;
                for (int i = 0; i < n; ++i)
                    inFrame = inFrame && gV[i].x >= (float)fr.x - 0.5f && gV[i].x <= (float)(fr.x + fr.w) && gV[i].y >= (float)fr.y + (float)fr.h * 0.85f &&
                              gV[i].y <= (float)(fr.y + fr.h);
                Check(inFrame, "the banner stays in the lowest seventh of the frame", text);
            }
            const double t0 = NowUs();
            for (int k = 0; k < 40; ++k) BuildState(s, view, now, gV, kCap);
            const double us = (NowUs() - t0) / 40.0;
            if (us > worstBuildUs) worstBuildUs = us;
            if (everyPicture || demo == 1 || demo == 12) {
                Image img = Backdrop(size[0], size[1], &shot);
                BuildState(s, view, now, gV, kCap);  // the timing loop above built it last: the picture list is the same
                Raster(img, gV, picN ? split : n);
                if (picN) {
                    RasterPics(img, picList, picN, sheet);
                    Raster(img, gV + split, n - split);
                }
                snprintf(path, sizeof path, "%s\\%s_%dx%d_%02d.bmp", outDir, form == 2 ? "lookother" : form ? "tiles" : "stage", size[0], size[1], demo);
                if (form != 2) Check(SaveBmp(path, img), "picture written", path), ++pictures;  // the look's banner and long list are the plain ones again
            }
        }

        // The choice after a match and the re-fight's card, in both forms at every size: plainly (rectangles and the
        // HUD display's faces, where the game's art is not read or the look is switched off) and in MK9's look.
        for (int form = 0; form < 2; ++form) {
            ArenaPics::Adopt(&pics, form ? &art : nullptr);
            StageSkin::Reset();
            snprintf(text, sizeof text, "%dx%d", size[0], size[1]);
            if (form) Check(StageSkin::Prepare(ArenaPics::ArtOf(), view) && StageSkin::ReadyFor(view), "choice: MK9's lettering fits its texture at this size", text);
            const int cx = fr.x + fr.w / 2;
            const float foot = (float)fr.y + (float)fr.h * 0.85f;
            // The look's panel (StageScreen.cpp: 480 units to each side of the middle, from 250 to 826).
            const float px0 = (float)(cx - std::lround(480.0f * fr.u)), px1 = (float)(cx + std::lround(480.0f * fr.u));
            const float py0 = (float)(fr.y + std::lround(250.0f * fr.u)), py1 = (float)(fr.y + std::lround(826.0f * fr.u));
            for (int demo = kDemos + 1; demo <= kDemos + kVoteDemos; ++demo) {
                State s;
                const uint64_t now = 5000000ull + 1000ull * (uint64_t)demo;
                Check(Demo(demo, now, &s), "canned state of the choice after a match exists");
                const int n = BuildState(s, view, now, gV, kCap);
                const StageSkin::Vtx* lookV = nullptr;
                const int lookN = Look(&lookV);
                const Pic* picList = nullptr;
                int split = 0;
                snprintf(text, sizeof text, "%dx%d choice state %d %s: %d vertices, %d of the look", size[0], size[1], demo - kDemos, form ? "look" : "plain", n, lookN);
                if (demo == kDemos + 1 || demo == kDemos + kVoteDemos) puts(text);
                Check(n > 0 && n % 6 == 0 && n <= kCap && Dropped() == 0, "choice: a frame of whole rectangles, none dropped", text);
                Check(Pictures(&picList, &split) == 0, "choice: no picture tiles", text);
                Check((lookN > 0) == (form == 1) && lookN % 6 == 0, "choice: MK9's look where its art is, the plain panel where it is not", text);
                bool inside = true, low = true;
                for (int i = 0; i < n; ++i) {
                    inside = inside && gV[i].x >= -0.5f && gV[i].y >= -0.5f && gV[i].x <= (float)size[0] && gV[i].y <= (float)size[1];
                    low = low && gV[i].y >= foot;
                }
                Check(inside, "choice: every rectangle inside the picture", text);
                if (s.phase == kCard) Check(low, "the re-fight's card stays in the lowest seventh of the frame", text);
                else Check(gV[0].x <= 0.0f && gV[4].x >= (float)size[0] - 0.5f && gV[4].y >= (float)size[1] - 0.5f, "choice: the picture behind the panel is dimmed", text);
                if (form) {
                    // The plain rectangles: the dimming alone (the card: the middle of its band). Every textured
                    // rectangle: on the panel (the card: at the frame's foot), filled from inside the atlas, never from
                    // the arena pictures' quarter; a letter is drawn pixel for pixel.
                    bool placed = true, inAtlas = true, sharp = true, noPicture = true;
                    int letters = 0;
                    for (int i = 0; i + 6 <= lookN; i += 6) {
                        const StageSkin::Vtx& a = lookV[i];
                        const StageSkin::Vtx& e = lookV[i + 4];
                        placed = placed && a.x < e.x && a.y < e.y &&
                                 (s.phase == kCard ? a.y >= foot - 0.5f && e.y <= (float)(fr.y + fr.h) && a.x >= (float)fr.x - 0.5f && e.x <= (float)(fr.x + fr.w)
                                                   : a.x >= px0 - 0.5f && e.x <= px1 && a.y >= py0 - 0.5f && e.y <= py1);
                        for (int k = 0; k < 6; ++k) inAtlas = inAtlas && lookV[i + k].u >= 0.0f && lookV[i + k].u <= 1.0f && lookV[i + k].v >= 0.0f && lookV[i + k].v <= 1.0f;
                        if (a.v >= 0.5f) {  // the lower half: lettering
                            ++letters;
                            const float tw = std::abs(e.u - a.u) * (float)ArenaPics::kAtlasW, th = (e.v - a.v) * (float)ArenaPics::kAtlasH;
                            sharp = sharp && std::abs(tw - (e.x - a.x)) < 0.01f && std::abs(th - (e.y - a.y)) < 0.01f && e.v <= 1.0f;
                        } else {
                            noPicture = noPicture && std::max(a.u, e.u) > 0.5f;
                        }
                    }
                    snprintf(text + strlen(text), sizeof text - strlen(text), ", %d letters", letters);
                    Check(n == 6, "choice look: one plain rectangle (the dimming; the card: its band's middle)", text);
                    Check(placed, s.phase == kCard ? "choice look: the card stays at the frame's foot" : "choice look: every rectangle stays on the panel", text);
                    Check(inAtlas && noPicture, "choice look: every rectangle is filled from the atlas, none from an arena's picture", text);
                    Check(sharp && letters > (s.phase == kCard ? 30 : 100), "choice look: every letter is drawn pixel for pixel", text);
                    if (lookN > worstLook) worstLook = lookN;
                }
                const double t0 = NowUs();
                for (int k = 0; k < 40; ++k) BuildState(s, view, now, gV, kCap);
                const double us = (NowUs() - t0) / 40.0;
                if (us > worstBuildUs) worstBuildUs = us;
                if (everyPicture) {
                    Image img = Backdrop(size[0], size[1], &shot);
                    Raster(img, gV, n);
                    if (lookN) RasterLook(img, lookV, lookN, sheet);
                    snprintf(path, sizeof path, "%s\\%s_%dx%d_%02d.bmp", outDir, form ? "choicelook" : "choice", size[0], size[1], demo - kDemos);
                    Check(SaveBmp(path, img), "picture written", path), ++pictures;
                }
            }
            if (!form) continue;
            // The other player's cursor in the look: red and whole while its packets arrive, faint once they stop (a
            // frame of four rectangles and the three letters of its tag).
            State live, quiet, none, four;
            Demo(kDemos + 1, 1, &live);
            quiet = live, quiet.peer = kPeerQuiet;
            none = live, none.theirCursor = kNoCell;
            const auto faint = [&](const State& st, int* total) {
                BuildState(st, view, 1, gV, kCap);
                const StageSkin::Vtx* v = nullptr;
                const int n = Look(&v);
                int c = 0;
                for (int i = 0; i < n; ++i) c += (v[i].color >> 24) == 90;
                *total = n;
                return c;
            };
            int nLive = 0, nQuiet = 0, nNone = 0;
            const int fLive = faint(live, &nLive), fQuiet = faint(quiet, &nQuiet), fNone = faint(none, &nNone);
            snprintf(text, sizeof text, "%dx%d: live %d vertices (%d faint), no signal %d (%d faint), no cursor %d", size[0], size[1], nLive, fLive, nQuiet, fQuiet, nNone);
            Check(fLive == 0 && fNone == 0 && fQuiet == 42 && nLive - nNone == 42, "choice look: the other player's cursor: whole while live, faint without signal", text);
            // More rows than the look's panel holds: the plain panel, whatever art there is.
            four = live, four.cells = 4;
            snprintf(four.name[3], sizeof four.name[3], "ONE MORE");
            const int nFour = BuildState(four, view, 1, gV, kCap);
            Check(nFour > 6 && Look(nullptr) == 0, "choice look: a fourth row is drawn by the plain panel", text);
        }
        StageSkin::Reset();

        // MK9's look: every one of the game's 28 names is whole under its tile, on one line or two.
        {
            ArenaPics::Adopt(&pics, &art);
            StageSkin::Reset();
            const bool built = StageSkin::Prepare(ArenaPics::ArtOf(), view);
            const Layout l = LayoutOf(29, true, view);
            const int maxW = LookNameRoom(l, view);
            int cut = 0, two = 0, widest = 0;
            for (int i = 1; i <= 28 && built; ++i) {
                char a[40], b[40], joined[84];
                const int lines = WrapLook(kNames[i].shown, maxW, a, b, sizeof a);
                snprintf(joined, sizeof joined, lines == 2 ? "%s %s" : "%s", a, b);
                const int wa = StageSkin::Width(StageSkin::kName, a), wb = StageSkin::Width(StageSkin::kName, b);
                cut += strcmp(joined, kNames[i].shown) != 0 || wa > maxW || wb > maxW;
                two += lines == 2;
                widest = std::max({widest, wa, wb});
            }
            snprintf(text, sizeof text, "%dx%d look: room %d px, widest line %d px, %d names on two lines, %d cut, capitals %d px", size[0], size[1], maxW, widest, two, cut,
                     StageSkin::CapOf(StageSkin::kName));
            puts(text);
            Check(built && cut == 0 && widest > 0, "look: every stage name is whole under its picture", text);
            // The other player's cursor in the look: red and whole while its packets arrive, faint once they stop.
            State live, quiet, none;
            Demo(1, 1, &live);
            quiet = live, quiet.peer = kPeerQuiet;
            none = live, none.theirCursor = kNoCell;
            const auto faint = [&](const State& st, int* total) {
                BuildState(st, view, 1, gV, kCap);
                const StageSkin::Vtx* v = nullptr;
                const int n = Look(&v);
                int c = 0;
                for (int i = 0; i < n; ++i) c += (v[i].color >> 24) == 90;
                *total = n;
                return c;
            };
            int nLive = 0, nQuiet = 0, nNone = 0;
            const int fLive = faint(live, &nLive), fQuiet = faint(quiet, &nQuiet), fNone = faint(none, &nNone);
            snprintf(text, sizeof text, "live %d vertices (%d faint), no signal %d (%d faint), no cursor %d", nLive, fLive, nQuiet, fQuiet, nNone);
            Check(fLive == 0 && fNone == 0 && fQuiet == 24 && nLive - nNone == 24, "look: the other player's cursor: whole while live, faint without signal", text);
            // A buffer of plain rectangles too small even for the dimming: nothing is written past it.
            static Hud::Vtx tiny[4];
            tiny[3].color = 0xC0FFEE11;
            Check(BuildState(live, view, 1, tiny, 3) == 0 && Dropped() > 0 && tiny[3].color == 0xC0FFEE11, "look: a full buffer drops and reports", text);
            StageSkin::Reset();
        }

        ArenaPics::Adopt(nullptr);
        // The grid: every cell on the panel, in the frame, the same size, none over another.
        for (const int which : {2, 3, 7, 29, 30, 31, 48, -2, -8, -9, -29, -32, -33}) {  // negative: as picture tiles
            const bool asTiles = which < 0;
            const int count = asTiles ? -which : which;
            const Layout l = LayoutOf(count, asTiles, view);
            snprintf(text, sizeof text, "%dx%d, %d cells%s", size[0], size[1], count, asTiles ? " as tiles" : "");
            if (asTiles) {
                const bool fits = count <= kTileCells;
                bool ok = l.tiles == fits && (!fits || l.cols == kTileCols);
                const int twoLines = 2 * Hud::CapOf(Hud::kSmallFace) + 2;
                for (int i = 0; i < count && ok && fits; ++i) {
                    const int w = l.pic[i].x1 - l.pic[i].x0, h = l.pic[i].y1 - l.pic[i].y0;
                    ok = Inside(l.pic[i], l.cell[i]) && h * 16 >= w * 9 - 16 && h * 16 <= w * 9 + 16 && w * 10 >= (l.cell[i].x1 - l.cell[i].x0) * 8 &&
                         l.cell[i].y1 - l.pic[i].y1 >= twoLines;
                }
                Check(ok, "tiles: 8 across up to 32 cells (the list above that), a 16:9 picture in each and room for two lines of its name", text);
            }
            const int cells = count;
            bool ok = Inside(l.panel, l.frame) && l.cols * l.rows >= cells && l.cols * (l.rows - 1) < cells;
            for (int i = 0; i < cells && ok; ++i) {
                ok = Inside(l.cell[i], l.panel) && l.cell[i].x1 - l.cell[i].x0 == l.cell[0].x1 - l.cell[0].x0 &&
                     l.cell[i].y1 - l.cell[i].y0 == l.cell[0].y1 - l.cell[0].y0;
                for (int j = 0; j < i && ok; ++j)
                    ok = l.cell[i].x0 >= l.cell[j].x1 || l.cell[j].x0 >= l.cell[i].x1 || l.cell[i].y0 >= l.cell[j].y1 || l.cell[j].y0 >= l.cell[i].y1;
            }
            Check(ok, "the grid's cells lie on the panel, equal and apart", text);
            // A row is tall enough for its face.
            Check(l.cell[0].y1 - l.cell[0].y0 >= Hud::CapOf(l.face) + 6, "a row holds a line of its face", text);
        }
        // Under a tile every one of the game's 28 names is whole, on one line or on two.
        {
            const Layout l = LayoutOf(29, true, view);
            const int maxW = l.cell[0].x1 - l.cell[0].x0 - 2 * ((int)(3.0f * fr.u + 1.5f) + (int)(4.0f * fr.u + 1.0f));
            int cut = 0, two = 0, widest = 0;
            for (int i = 1; i <= 28; ++i) {
                char a[40], b[40], joined[84];
                const int lines = Wrap(Hud::kSmallFace, kNames[i].shown, maxW, a, b, sizeof a);
                snprintf(joined, sizeof joined, lines == 2 ? "%s %s" : "%s", a, b);
                cut += strcmp(joined, kNames[i].shown) != 0 || Hud::Width(Hud::kSmallFace, a) > maxW || Hud::Width(Hud::kSmallFace, b) > maxW;
                two += lines == 2;
                widest = std::max({widest, Hud::Width(Hud::kSmallFace, a), Hud::Width(Hud::kSmallFace, b)});
            }
            snprintf(text, sizeof text, "%dx%d: room %d px, widest line %d px, %d names on two lines, %d cut", size[0], size[1], maxW, widest, two, cut);
            puts(text);
            Check(cut == 0, "tiles: every stage name is whole under its picture", text);
            char a[40], b[40];
            Check(Wrap(Hud::kSmallFace, "SHANG TSUNG GARDENS NIGHT", Hud::Width(Hud::kSmallFace, "GARDENS NIGHT"), a, b, sizeof a) == 2 && !strcmp(a, "SHANG TSUNG") &&
                      !strcmp(b, "GARDENS NIGHT"),
                  "a name is broken where its two lines are most alike");
            Check(Wrap(Hud::kSmallFace, "WWWWWWWWWWWWWWWWWWWWWWWWWWWW", maxW, a, b, sizeof a) == 1 && Hud::Width(Hud::kSmallFace, a) <= maxW && strlen(a) < 28 && !b[0],
                  "one word wider than its tile is cut");
            Check(Wrap(Hud::kSmallFace, nullptr, maxW, a, b, sizeof a) == 1 && !a[0] && !b[0], "no name, no lines");
        }
        // The game's 28 names and RANDOM fit their cells whole, also beside a player's mark.
        {
            const Layout l = LayoutOf(29, false, view);
            const int cellW = l.cell[0].x1 - l.cell[0].x0;
            const int fixed = 2 * (int)(3.0f * fr.u + 1.5f) + (int)(18.0f * fr.u + 1.0f) + 2 * (int)(12.0f * fr.u + 1.0f);
            int widest = 0, cut = 0, cutMarked = 0;
            const int mark = Hud::Width(Hud::kSmallFace, "BOTH");
            for (int i = 1; i <= 28; ++i) {
                const int w = Hud::Width(l.face, kNames[i].shown);
                if (w > widest) widest = w;
                cut += w + fixed > cellW;
                cutMarked += w + fixed + mark > cellW;
            }
            printf("%dx%d: cell %d px wide, widest name %d px, room beside it %d px (a mark needs %d)\n", size[0], size[1], cellW, widest,
                   cellW - fixed - widest, mark);
            snprintf(text, sizeof text, "%dx%d: %d names cut, %d cut beside a mark", size[0], size[1], cut, cutMarked);
            Check(cut == 0 && cutMarked == 0, "every stage name fits its cell whole, with a mark beside it", text);
        }
        // A full buffer drops rectangles and says so; it never writes past the end.
        {
            State s;
            Demo(12, 1000, &s);
            static Hud::Vtx small[601];
            small[600].color = 0xC0FFEE11;
            const int n = BuildState(s, view, 1000, small, 600);
            Check(n <= 600 && Dropped() > 0 && small[600].color == 0xC0FFEE11, "a full buffer drops and reports", text);
        }
    }

    // A state the rendering thread caught half written, or with values no flow sets: drawn without leaving the picture.
    {
        const Hud::View view{0, 0, 1280, 720};
        Hud::Reset();
        Hud::Prepare(view);
        State s;
        memset(&s, 0x58, sizeof s);  // every byte 'X': no text ends, every index is 88
        s.phase = kPicking;
        int n = BuildState(s, view, 1, gV, kCap);
        bool inside = n > 0 && n <= kCap;  // 48 cells of the costliest letter may fill the buffer: dropped, never overrun
        for (int i = 0; i < n; ++i) inside = inside && gV[i].x >= -0.5f && gV[i].y >= -0.5f && gV[i].x <= 1280.0f && gV[i].y <= 720.0f;
        snprintf(text, sizeof text, "%d vertices, %d dropped", n, Dropped());
        Check(inside, "a state of nonsense bytes is drawn inside the picture", text);
        // The same nonsense with a number of cells the tiles hold: tiles, and no picture from a place that does not exist.
        s.cells = 29;
        n = BuildState(s, view, 1, gV, kCap);
        inside = n > 0 && n <= kCap;
        for (int i = 0; i < n; ++i) inside = inside && gV[i].x >= -0.5f && gV[i].y >= -0.5f && gV[i].x <= 1280.0f && gV[i].y <= 720.0f;
        const Pic* none = nullptr;
        int splitAt = -1;
        const int nonsensePics = Pictures(&none, &splitAt);
        snprintf(text, sizeof text, "%d vertices, %d dropped, %d pictures, split %d", n, Dropped(), nonsensePics, splitAt);
        Check(inside && nonsensePics == 0 && splitAt > 0 && splitAt <= n, "nonsense as tiles: inside the picture, and no picture from outside the sheet", text);
        s.pic[3] = 200, s.pic[4] = 33, s.pic[5] = 32;
        BuildState(s, view, 1, gV, kCap);
        Check(Pictures(&none, nullptr) == 1 && none[0].slot == 31, "a picture's place past the sheet's 32 is left out");
        s.phase = 200;
        Check(BuildState(s, view, 1, gV, kCap) == 0, "a phase that does not exist draws nothing");
        State off;
        Check(BuildState(off, view, 1, gV, kCap) == 0, "no state, no drawing");
        const Hud::View other{0, 0, 800, 600};
        Demo(1, 1, &s);
        Check(BuildState(s, other, 1, gV, kCap) == 0, "no drawing for a size the faces were not built for");

        // The other player's cursor: an outline while its packets arrive, the same outline faint once they stop (so a
        // cursor that no longer follows the other player is not shown as if it did), a filled cell once it is locked.
        {
            const auto faint = [&](const State& st, int* total) {
                const int n = BuildState(st, view, 1, gV, kCap);
                int c = 0;
                for (int i = 0; i < n; ++i) c += (gV[i].color >> 24) == 90;
                *total = n;
                return c;
            };
            for (int form = 0; form < 2; ++form) {
            ArenaPics::Adopt(form ? &pics : nullptr);
            State live, quiet, none;
            Demo(1, 1, &live);
            quiet = live;
            quiet.peer = kPeerQuiet;
            none = live;
            none.theirCursor = kNoCell;
            int nLive = 0, nQuiet = 0, nNone = 0;
            const int fLive = faint(live, &nLive), fQuiet = faint(quiet, &nQuiet), fNone = faint(none, &nNone);
            snprintf(text, sizeof text, "live %d vertices (%d faint), no signal %d (%d faint), no cursor %d", nLive, fLive, nQuiet, fQuiet, nNone);
            Check(fLive == 0 && fNone == 0 && fQuiet > 0 && fQuiet % 6 == 0 && fQuiet == nLive - nNone,
                  form ? "tiles: the other player's cursor: whole while live, faint without signal" : "the other player's cursor: whole while live, faint without signal", text);
            }
            ArenaPics::Adopt(nullptr);
            State demo13;
            Check(Demo(13, 1, &demo13) && demo13.peer == kPeerQuiet && demo13.theirCursor != kNoCell, "canned state 13 is the cursor without signal");
        }

        // The published state and the canned one, through the rendering thread's entry.
        SetDemo(0);
        Publish(off);
        Check(Build(view, true, 10, gV, kCap) == 0 && CanDraw(), "nothing published: nothing drawn; the panel could be drawn");
        Demo(2, 10, &s);
        Publish(s);
        const int pub = Build(view, true, 10, gV, kCap);
        Check(pub > 0, "a published state is drawn");
        Publish(off);
        Check(Build(view, true, 10, gV, kCap) == 0, "and gone when it is taken back");
        SetDemo(5);
        Check(DemoShown() == 5 && Build(view, true, 10, gV, kCap) > 0, "'stagedemo 5' draws without a published state");
        SetDemo(99);
        Check(DemoShown() == 0 && Build(view, true, 10, gV, kCap) == 0, "a canned state that does not exist is 'off'");
        Check(Build(view, false, 10, gV, kCap) == 0 && !CanDraw(), "without faces nothing is drawn and the flow is told so");
        long draws = 0, largest = 0, dropped = 0;
        Counters(&draws, &largest, &dropped);
        Check(draws == 2 && largest >= pub && dropped == 0, "the draws are counted");
    }

    // The roulette: fast, then slower, and it ends on the winner on both screens (the same seed, mirrored winners).
    {
        bool ends = true, counts = true, slows = true, mirrored = true;
        for (uint32_t seed = 0; seed < 6; ++seed)
            for (const uint8_t winner : {(uint8_t)kMine, (uint8_t)kTheirs}) {
                State s, o;
                s.phase = o.phase = kRoulette, s.seed = o.seed = seed, s.phaseSinceMs = o.phaseSinceMs = 1000;
                s.winner = winner;
                o.winner = winner == kMine ? kTheirs : kMine;
                int changes = 0, last = RouletteLit(s, 1000), firstGap = 0, lastGap = 0, since = 0;
                for (uint32_t t = 1; t <= kRouletteMs + 50; ++t) {
                    const int lit = RouletteLit(s, 1000 + t);
                    mirrored &= lit == 1 - RouletteLit(o, 1000 + t);
                    ++since;
                    if (lit != last) {
                        if (!changes) firstGap = since;
                        lastGap = since;
                        since = 0;
                        ++changes;
                        last = lit;
                    }
                }
                ends &= last == (winner == kTheirs ? 1 : 0) && RouletteLit(s, 1000 + kRouletteMs) == last && RouletteLit(s, 99999999) == last;
                counts &= changes == 9 + 2 * (int)(seed % 3);
                slows &= lastGap > 3 * firstGap && firstGap >= 30;
            }
        Check(ends, "the roulette ends on the winner and stays there");
        Check(counts, "it changes 9, 11 or 13 times, as the shared number says");
        Check(slows, "the first change comes after 30 ms or more and the last one takes three times as long");
        Check(mirrored, "the two screens light the same pick at every millisecond");
        State s;
        s.phase = kRoulette, s.winner = kMine, s.phaseSinceMs = 5000;
        Check(RouletteLit(s, 100) == RouletteLit(s, 5000), "a clock before the phase began is the phase's first moment");
    }

    // The cursor: always on a cell, every cell reachable, a full turn comes back.
    {
        bool onCell = true, reach = true, turns = true;
        for (int form = 0; form < 2; ++form)
        for (int cells = 1; cells <= kMaxCells; ++cells) {
            const bool tiles = form != 0;  // above 32 cells the tiles are the list again
            bool seen[kMaxCells] = {};
            uint8_t todo[kMaxCells];
            int n = 0;
            seen[0] = true;
            todo[n++] = 0;
            while (n) {
                const uint8_t at = todo[--n];
                for (int d = 0; d < 4; ++d) {
                    const uint8_t to = Move(cells, tiles, at, d == 0 ? -1 : d == 1 ? 1 : 0, d == 2 ? -1 : d == 3 ? 1 : 0);
                    onCell &= to < cells;
                    if (to < cells && !seen[to]) {
                        seen[to] = true;
                        todo[n++] = to;
                    }
                }
            }
            for (int i = 0; i < cells; ++i) reach &= seen[i];
            const Layout l = LayoutOf(cells, tiles, Hud::View{0, 0, 1280, 720});
            for (int i = 0; i < cells; ++i) {
                uint8_t at = (uint8_t)i;
                for (int k = 0; k < l.rows; ++k) at = Move(cells, tiles, at, 0, 1);
                // A column of a short last row has one cell less: a full turn of it is one step shorter.
                const bool shortColumn = (l.rows - 1) * l.cols + i % l.cols >= cells;
                if (!shortColumn) turns &= at == i;
            }
            onCell &= Move(cells, tiles, 200, 1, 0) == cells - 1 && Move(cells, tiles, kNoCell, 0, 0) == cells - 1;
        }
        Check(onCell, "a step always lands on a cell (1 to 48 cells, both forms; a cursor from nowhere goes to the last cell, RANDOM)");
        Check(reach, "every cell can be reached with the four directions");
        Check(turns, "as many steps down as there are rows come back to the same cell");
        Check(Move(0, false, 0, 1, 0) == kNoCell && Move(0, true, 0, 1, 0) == kNoCell, "no cells, no cursor");
        Check(Move(29, false, 0, -1, 0) == 2 && Move(29, false, 2, 1, 0) == 0 && Move(29, false, 0, 0, -1) == 27 && Move(29, false, 28, 0, 1) == 1 &&
                  Move(29, false, 28, 1, 0) == 27,
              "29 cells in 3 columns: the edges wrap, and the short last row is stepped over");
        Check(Move(29, true, 0, -1, 0) == 7 && Move(29, true, 7, 1, 0) == 0 && Move(29, true, 0, 0, -1) == 24 && Move(29, true, 28, 0, 1) == 4 &&
                  Move(29, true, 28, 1, 0) == 24 && Move(29, true, 5, 0, -1) == 21 && Move(29, true, 21, 0, 1) == 5 && Move(29, true, 12, 1, 0) == 13,
              "29 tiles in 8 columns: the edges wrap, and the short last row is stepped over");
        Check(Move(40, true, 0, -1, 0) == Move(40, false, 0, -1, 0) && Move(40, true, 0, -1, 0) == 3, "more cells than the tiles hold move as the list does");
    }

    // The names.
    {
        bool ok = true;
        for (const Named& n : kNames) {
            char out[40];
            DisplayName(n.table, out, sizeof out);
            if (strcmp(out, n.shown)) {
                ok = false;
                printf("  \"%s\" gives \"%s\", wanted \"%s\"\n", n.table, out, n.shown);
            }
        }
        Check(ok, "the stage table's names read as words (41 names)");
        char tiny[8];
        memset(tiny, 0x7F, sizeof tiny);
        DisplayName("BGND_STGardensN", tiny, 6);
        Check(strlen(tiny) == 5 && tiny[6] == 0x7F, "a short buffer is filled and not overrun");
        char none[4] = "abc";
        DisplayName(nullptr, none, sizeof none);
        Check(!strcmp(none, "?"), "no name at all is a question mark");
    }

    printf("largest frame %d vertices of %d, slowest build %.0f us, %d pictures; MK9's look: largest frame %d textured vertices of 30000, slowest lettering %.1f ms\n",
           worstVerts, kCap, worstBuildUs, pictures, worstLook, worstFacesUs / 1000.0);
    Check(worstVerts < kCap * 3 / 4, "the largest frame leaves a quarter of the buffer free");
    Check(worstLook > 0 && worstLook < 30000 / 2, "the look's largest frame leaves half of its buffer free");
    Check(worstFacesUs < 80000.0, "the look's lettering is scaled for a picture size in under 80 ms (3 ms at 1080 lines, 7 to 25 at 2160)");
    Check(worstBuildUs < 3000.0, "a frame builds in under three milliseconds");
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
