// Host test of src\debug\ScoreView.cpp: the score in a fight (each player's wins today under that player's health bar,
// the wins in a row of the one on a run) without the game. For each picture size and each canned badge it builds the
// rectangles, checks them (inside the frame, under the health bars, clear of the HUD's Wi-Fi marks and of the group
// under the round timer, the two sides apart, none dropped), checks the fades, and draws them over a background into
// build\hosttests\scoreview\*.bmp so the look can be judged by eye.
//   scoreview_test.exe <out folder> [background.bmp]
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../src/debug/Hud.h"
#include "../../src/debug/ScoreView.h"

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

// The game's picture: black, with the 16:9 frame holding the background (a screenshot of a fight when one is given).
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
                // A screenshot of a 16:10 window holds its 16:9 frame in the middle rows: take those.
                const int sfh = shot->w * 9 / 16 <= shot->h ? shot->w * 9 / 16 : shot->h, sfy = (shot->h - sfh) / 2;
                const int sx = (int)((long long)x * shot->w / fw), sy = sfy + (int)((long long)y * sfh / fh);
                memcpy(p, &shot->px[((size_t)sy * shot->w + sx) * 3], 3);
            } else {
                int v = 30 + x * 200 / fw;
                if (y > fh * 12 / 100 && y < fh * 14 / 100) v = 250;
                p[0] = (uint8_t)(v * 9 / 10), p[1] = (uint8_t)v, p[2] = (uint8_t)(v * 8 / 10);
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

constexpr int kCap = 96000;  // Overlay.cpp's kBadgeMaxV
Hud::Vtx gV[kCap];

struct Bounds {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    void Add(float x, float y) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
};
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* outDir = argc > 1 ? argv[1] : ".";
    Image shot;
    if (argc > 2 && !LoadBmp(argv[2], shot)) printf("background %s not readable; using the gradient\n", argv[2]);
    CreateDirectoryA(outDir, nullptr);
    const int sizes[][2] = {{1920, 1080}, {1920, 1200}, {1280, 720}, {2560, 1440}, {3840, 2160}, {960, 540}, {1024, 768}};
    int largest = 0;
    for (const auto& size : sizes) {
        Hud::Reset();
        ScoreView::Reset();
        const Hud::View view{0, 0, size[0], size[1]};
        char what[160];
        snprintf(what, sizeof what, "%dx%d", size[0], size[1]);
        Check(Hud::Prepare(view), "faces built", what);
        Check(Hud::BigFace(), "the score's face built", what);
        const Hud::Frame fr = Hud::FrameOf(view);
        const float u = fr.u;
        for (int demo = 1; demo <= ScoreView::kDemos; ++demo) {
            ScoreView::Badge b;
            Check(ScoreView::Demo(demo, &b), "demo exists");
            const int n = ScoreView::BuildBadge(b, view, 255, gV, kCap);
            snprintf(what, sizeof what, "%dx%d demo %d: %d vertices", size[0], size[1], demo, n);
            Check(n > 0 && n % 6 == 0, "rectangles built", what);
            Check(ScoreView::Dropped() == 0, "none dropped", what);
            if (n > largest) largest = n;
            Bounds side[2];
            for (int i = 0; i < n; ++i) {
                const float x = gV[i].x + 0.5f, y = gV[i].y + 0.5f;
                side[x < fr.x + fr.w / 2 ? 0 : 1].Add(x, y);
            }
            for (int s = 0; s < 2; ++s) {
                const Bounds& k = side[s];
                char d[200];
                snprintf(d, sizeof d, "%s side %d: x %.0f..%.0f y %.0f..%.0f (frame units x %.0f..%.0f y %.0f..%.0f)", what, s + 1, k.x0, k.x1, k.y0, k.y1,
                         (k.x0 - fr.x) / u, (k.x1 - fr.x) / u, (k.y0 - fr.y) / u, (k.y1 - fr.y) / u);
                Check(k.x1 > k.x0, "each side has its numbers", d);
                Check(k.x0 >= fr.x && k.x1 <= fr.x + fr.w && k.y0 >= fr.y && k.y1 <= fr.y + fr.h, "inside the frame", d);
                // Under the health bar (it ends 95 units down) and the Wi-Fi mark (its baseline 130), above the fighters' heads.
                Check(k.y0 >= fr.y + 131.0f * u, "under the health bar and the Wi-Fi mark", d);
                Check(k.y1 <= fr.y + 240.0f * u, "in the strip under the HUD", d);
                // Clear of the group under the round timer and the round-win marks (the middle 300 units each way).
                if (s == 0) Check(k.x1 <= fr.x + fr.w / 2 - 300.0f * u, "left of the timer's group", d);
                else Check(k.x0 >= fr.x + fr.w / 2 + 300.0f * u, "right of the timer's group", d);
                // In from the side as far as the fighter's name (the portrait is there).
                if (s == 0) Check(k.x0 >= fr.x + 210.0f * u, "beside the portrait", d);
                else Check(k.x1 <= fr.x + fr.w - 210.0f * u, "beside the portrait", d);
            }
            Image img = Backdrop(size[0], size[1], &shot);
            Raster(img, gV, n);
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\badge_%dx%d_%d.bmp", outDir, size[0], size[1], demo);
            Check(SaveBmp(path, img), "picture written", path);
        }
        // The published badge: shown at once on the first frame, faded out after it is withdrawn, never left on.
        ScoreView::SetDemo(0);
        ScoreView::Badge on;
        ScoreView::Demo(1, &on);
        ScoreView::Publish(on);
        Check(ScoreView::Build(view, true, 10000, gV, kCap) > 0, "published: drawn", what);
        Check(ScoreView::Build(view, false, 10016, gV, kCap) == 0, "no faces: nothing", what);
        ScoreView::Publish(ScoreView::Badge{});
        Check(ScoreView::Build(view, true, 10032, gV, kCap) > 0, "withdrawn: still fading out", what);
        Check(ScoreView::Build(view, true, 10400, gV, kCap) == 0, "withdrawn: gone after the fade", what);
        ScoreView::SetDemo(3);
        Check(ScoreView::Build(view, true, 10416, gV, kCap) > 0, "a demo shows without a published badge", what);
        ScoreView::SetDemo(0);
        Check(ScoreView::Build(view, true, 11000, gV, kCap) == 0, "demo off: nothing", what);
    }
    printf("  largest badge %d vertices of %d\n", largest, kCap);
    Check(largest * 2 <= kCap, "the badge buffer has room to spare (twice the largest)");
    printf("  %d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
