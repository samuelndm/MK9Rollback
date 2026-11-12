// Host test of src\debug\Hud.cpp: the HUD display without the game. For each picture size and each canned state it
// builds the frame's rectangles, checks them (inside the picture, within the buffer, none dropped, fades end), times the
// build, and draws them over a background into build\hosttests\hud\*.bmp so the look can be judged by eye.
//   hud_test.exe <out folder> [background.bmp]
// The background is a screenshot of the game (24 or 32 bit BMP), scaled to each size's 16:9 frame; without one, a
// gradient with a bright and a dark band.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../src/debug/Hud.h"

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

// The picture the game would have drawn: black, with the 16:9 frame holding the background.
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
            } else {  // left to right dark to bright, a white band and a dark band across the HUD's rows
                int v = 30 + x * 200 / fw;
                if (y > fh * 12 / 100 && y < fh * 14 / 100) v = 250;
                if (y > fh * 15 / 100 && y < fh * 17 / 100) v = 8;
                p[0] = (uint8_t)(v * 9 / 10), p[1] = (uint8_t)v, p[2] = (uint8_t)(v * 8 / 10);
            }
        }
    return img;
}

// Direct3D 9's rule for the rectangles Hud makes (corners at pixel - 0.5): whole pixels, blended source-over.
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

constexpr int kCap = 96000;
Hud::Vtx gV[kCap];

double NowUs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* outDir = argc > 1 ? argv[1] : ".";
    Image shot;
    if (argc > 2 && !LoadBmp(argv[2], shot)) printf("background %s not readable; using the gradient\n", argv[2]);
    CreateDirectoryA(outDir, nullptr);
    static const int kSizes[][2] = {{1920, 1080}, {1920, 1200}, {1280, 720}, {1024, 768}, {960, 600}, {2560, 1440}, {3840, 2160}, {3440, 1440}};
    char text[256], path[MAX_PATH];
    int worstVerts = 0;
    double worstBuildUs = 0, worstPrepareMs = 0;
    for (const auto& size : kSizes) {
        const Hud::View view{0, 0, size[0], size[1]};
        Hud::Reset();
        const double p0 = NowUs();
        const bool ok = Hud::Prepare(view);
        const double prepareMs = (NowUs() - p0) / 1000.0;
        if (prepareMs > worstPrepareMs) worstPrepareMs = prepareMs;
        Hud::Describe(text, sizeof text);
        printf("%dx%d: %s, %.1f ms\n", size[0], size[1], text, prepareMs);
        snprintf(text, sizeof text, "%dx%d", size[0], size[1]);
        Check(ok, "faces built", text);
        if (!ok) continue;
        Check(Hud::Prepare(view), "a second Prepare for the same size keeps the faces", text);
        for (int demo = 1; demo <= Hud::kDemos; ++demo) {
            Overlay::Stats s;
            Check(Hud::Demo(demo, &s), "demo exists");
            // A second of frames: the fades are through. The clock starts fresh for each case.
            uint64_t now = 1000000ull * (uint64_t)demo;
            int n = 0;
            for (int f = 0; f < 60; ++f, now += 16) n = Hud::Build(s, view, now, gV, kCap);
            snprintf(text, sizeof text, "%dx%d demo %d: %d vertices", size[0], size[1], demo, n);
            Check(n > 0 && n % 6 == 0 && n <= kCap, "a frame of whole rectangles", text);
            Check(Hud::Dropped() == 0, "no rectangle dropped", text);
            if (n > worstVerts) worstVerts = n;
            bool inside = true;
            for (int i = 0; i < n; ++i)
                inside = inside && gV[i].x >= -0.5f && gV[i].y >= -0.5f && gV[i].x <= (float)size[0] && gV[i].y <= (float)size[1];
            Check(inside, "every rectangle inside the picture", text);
            // In a fight the group's row (136..162 units below the 16:9 frame's top) leaves the round timer's spike
            // free: no rectangle within 3 units of the frame's middle, whatever the numbers' widths.
            if (s.phase == Overlay::kPhaseFight) {
                const int fh = (long long)size[0] * 9 >= (long long)size[1] * 16 ? size[1] : size[0] * 9 / 16;
                const float u = (float)fh / 1080.0f, cx = (float)size[0] / 2.0f, fy = (float)(size[1] - fh) / 2.0f;
                bool clear = true;
                for (int i = 0; i + 5 < n; i += 6) {
                    float x0 = gV[i].x, x1 = gV[i].x, y0 = gV[i].y, y1 = gV[i].y;
                    for (int k = 1; k < 6; ++k) {
                        const Hud::Vtx& v = gV[i + k];
                        x0 = v.x < x0 ? v.x : x0, x1 = v.x > x1 ? v.x : x1, y0 = v.y < y0 ? v.y : y0, y1 = v.y > y1 ? v.y : y1;
                    }
                    if (y1 > fy + 136.0f * u && y0 < fy + 162.0f * u && x1 > cx - 3.0f * u && x0 < cx + 3.0f * u) clear = false;
                }
                Check(clear, "the group's row leaves the timer's spike free", text);
            }
            const double t0 = NowUs();
            for (int k = 0; k < 200; ++k) Hud::Build(s, view, now, gV, kCap);
            const double us = (NowUs() - t0) / 200.0;
            if (us > worstBuildUs) worstBuildUs = us;
            Image img = Backdrop(size[0], size[1], &shot);
            Raster(img, gV, n);
            snprintf(path, sizeof path, "%s\\hud_%dx%d_%02d.bmp", outDir, size[0], size[1], demo);
            Check(SaveBmp(path, img), "picture written", path);
            // The state goes away: the display fades and is gone within half a second.
            Overlay::Stats off;
            int left = n;
            for (int f = 0; f < 32; ++f, now += 16) left = Hud::Build(off, view, now, gV, kCap);
            Check(left == 0, "gone half a second after its state ended", text);
        }
        // Hidden with the game's HUD (a finisher), and nothing at all without a state.
        Overlay::Stats s;
        Hud::Demo(1, &s);
        s.hudHidden = 1;
        int n = 0;
        uint64_t now = 77000000ull;
        for (int f = 0; f < 60; ++f, now += 16) n = Hud::Build(s, view, now, gV, kCap);
        Check(n == 0, "nothing while the game's HUD is hidden", text);
        // A full buffer drops rectangles and says so; it never writes past the end.
        Hud::Demo(12, &s);
        static Hud::Vtx small[601];
        small[600].color = 0xC0FFEE11;
        for (int f = 0; f < 60; ++f, now += 16) n = Hud::Build(s, view, now, small, 600);
        Check(n <= 600 && Hud::Dropped() > 0 && small[600].color == 0xC0FFEE11, "a full buffer drops and reports", text);
    }
    // The size changed and Prepare was not called: nothing is drawn with the other size's faces.
    {
        Overlay::Stats s;
        Hud::Demo(1, &s);
        const Hud::View other{0, 0, 800, 600};
        Check(Hud::Build(s, other, 5, gV, kCap) == 0, "no drawing for a size the faces were not built for");
    }
    // A size that changes again right after a build waits two seconds (a game flipping between two viewports must not
    // cost a face build per frame); the size the faces were built for keeps working meanwhile.
    {
        Overlay::Stats s;
        Hud::Demo(1, &s);
        const Hud::View a{0, 0, 1280, 720}, b{0, 0, 1600, 900};
        Hud::Reset();
        bool wait = true;
        Check(Hud::Prepare(a, &wait) && !wait, "after a reset the faces are built at once");
        const int builds = Hud::Builds();
        Check(!Hud::Prepare(b, &wait) && wait && Hud::Builds() == builds, "another size right after a build waits");
        Check(Hud::Build(s, b, 5, gV, kCap) == 0, "nothing is drawn for the waiting size");
        Check(Hud::Prepare(a, &wait) && !wait && Hud::Builds() == builds, "the built size still works, without a rebuild");
        Check(Hud::Build(s, a, 5, gV, kCap) > 0, "and is drawn");
        Sleep(2100);
        Check(Hud::Prepare(b, &wait) && !wait && Hud::Builds() == builds + 1, "two seconds later the other size is built");
        Check(Hud::Build(s, b, 2200, gV, kCap) > 0, "and is drawn then");
    }
    // The connection quality rule the bars use (it moved here from Overlay.cpp unchanged).
    {
        Overlay::Stats s;
        s.rttMs = 33;
        Check(Hud::Quality(s) == 0, "33 ms is good");
        s.rttMs = 61;
        Check(Hud::Quality(s) == 1, "61 ms is so-so");
        s.rttMs = 121;
        Check(Hud::Quality(s) == 2, "121 ms is bad");
        s.rttMs = 20, s.maxDepth = 3;
        Check(Hud::Quality(s) == 1, "3 frames rolled back is so-so");
        s.maxDepth = 5;
        Check(Hud::Quality(s) == 2, "5 frames rolled back is bad");
        s.maxDepth = 0, s.netStallMs = 600, s.netStallAgoS = 9;
        Check(Hud::Quality(s) == 0, "an old stall no longer counts");
        Check(!strcmp(Hud::StopText("peer disconnected (timeout)"), "Opponent disconnected"), "stop text mapped");
        Check(!strcmp(Hud::StopText("something new"), "something new"), "unknown stop text kept");
    }
    printf("largest frame %d vertices of %d, slowest build %.0f us, slowest face build %.1f ms\n", worstVerts, kCap, worstBuildUs, worstPrepareMs);
    Check(worstVerts < kCap * 3 / 4, "the largest frame leaves a quarter of the buffer free");
    Check(worstBuildUs < 1000.0, "a frame builds in under a millisecond");
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
