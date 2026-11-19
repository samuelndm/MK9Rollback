#include "ScoreView.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
using Hud::Canvas;
using Hud::Frame;
using ScoreView::Badge;

// Where, in 1/1080 of the 16:9 frame's height (the HUD display's units, Hud.cpp): the fighter's name on the health bar
// starts 219 in from the frame's side and the bar ends at 95; the HUD's Wi-Fi mark stands under the bar at 214 with its
// baseline at 130. The numbers stand under that mark, in from the side as far as the name.
constexpr float kSideX = 222.0f;     // from the frame's side to the badge's outer edge
constexpr float kNumberBase = 180.0f;  // the numbers' baseline
constexpr float kRunBase = 216.0f;     // the line of a run's baseline
constexpr float kGap = 12.0f;          // between the number and its word

constexpr uint32_t kNumber = 0xFFE38A, kWord = 0xE9D9A6, kRun = 0xFF9A3C;

int U(const Frame& f, float units) { return (int)std::lround(units * f.u); }

Badge gBuf[2];
volatile LONG gFront = 0, gDemo = 0;
int gDropped = 0;

// Render thread only: what is shown while it fades out, and the fade.
Badge gShown;
float gAlpha = 0;
uint64_t gLast = 0;

float Step(float a, bool on, float dtMs) {
    if (dtMs > 400.0f) return on ? 1.0f : 0.0f;  // the first frame, or a display that was away: no slow fade from stale state
    return on ? std::min(1.0f, a + dtMs / 160.0f) : std::max(0.0f, a - dtMs / 240.0f);
}

// One player's numbers: 'left' for player 1 (written from the left edge out), else player 2 (ending at the right edge).
void DrawSide(Canvas& c, const Frame& fr, bool left, unsigned wins, unsigned run, int alpha) {
    char number[12], run3[24];
    snprintf(number, sizeof number, "%u", std::min(wins, 9999u));
    const char* word = wins == 1 ? "WIN" : "WINS";
    const Hud::FaceId big = Hud::kBigFace;
    const int wNum = Hud::Width(big, number), wWord = Hud::Width(Hud::kMainFace, word), gap = U(fr, kGap);
    const int y = fr.y + U(fr, kNumberBase), yRun = fr.y + U(fr, kRunBase);
    const int edge = left ? fr.x + U(fr, kSideX) : fr.x + fr.w - U(fr, kSideX);
    if (left) {
        Hud::Write(c, big, number, edge, y, kNumber, alpha);
        Hud::Write(c, Hud::kMainFace, word, edge + wNum + gap, y, kWord, alpha);
    } else {
        Hud::Write(c, Hud::kMainFace, word, edge - wNum - gap - wWord, y, kWord, alpha);
        Hud::Write(c, big, number, edge - wNum, y, kNumber, alpha);
    }
    if (run >= 2) {
        snprintf(run3, sizeof run3, "%u IN A ROW", std::min(run, 999u));
        const int w = Hud::Width(Hud::kMainFace, run3);
        Hud::Write(c, Hud::kMainFace, run3, left ? edge : edge - w, yRun, kRun, alpha);
    }
}
}  // namespace

namespace ScoreView {

void Publish(const Badge& b) {
    if (!memcmp(&gBuf[gFront & 1], &b, sizeof b)) return;
    const LONG back = (gFront & 1) ^ 1;
    gBuf[back] = b;
    InterlockedExchange(&gFront, back);
}

int BuildBadge(const Badge& b, const Hud::View& v, int alpha, Hud::Vtx* out, int cap) {
    gDropped = 0;
    if (!Hud::FacesFor(v) || alpha <= 0) return 0;
    Canvas c{out, 0, cap, 0};
    const Frame fr = Hud::FrameOf(v);
    for (int side = 0; side < 2; ++side) DrawSide(c, fr, side == 0, b.wins[side], b.run[side], alpha);
    gDropped = c.dropped;
    return c.n;
}

int Build(const Hud::View& v, bool faces, uint64_t nowMs, Hud::Vtx* out, int cap) {
    Badge b = gBuf[gFront & 1];
    const int demo = gDemo;
    if (demo > 0 && !Demo(demo, &b)) b = Badge{};
    const float dt = gLast ? (float)(nowMs - gLast) : 1e9f;
    gLast = nowMs;
    if (b.on) gShown = b;
    gAlpha = Step(gAlpha, b.on != 0, dt);
    if (!faces || gAlpha <= 0) return 0;
    return BuildBadge(gShown, v, (int)(gAlpha * 255.0f + 0.5f), out, cap);
}

int Dropped() { return gDropped; }

bool Demo(int n, Badge* b) {
    *b = Badge{};
    b->on = 1;
    b->mySide = 1;
    switch (n) {
    case 1:  // an evening under way: player 2 on a run
        b->wins[0] = 13, b->wins[1] = 18, b->run[1] = 2;
        return true;
    case 2:  // this player on a run, from the right side
        b->mySide = 2, b->wins[0] = 4, b->wins[1] = 7, b->run[1] = 5;
        return true;
    case 3:  // the first match of the day
        return true;
    case 4:  // the widest: four digits each, a long run
        b->wins[0] = 8888, b->wins[1] = 9999, b->run[0] = 888;
        return true;
    default:
        *b = Badge{};
        return false;
    }
}
void SetDemo(int n) { InterlockedExchange(&gDemo, n < 0 || n > kDemos ? 0 : n); }
int DemoShown() { return gDemo; }
void Reset() {
    gShown = Badge{};
    gAlpha = 0;
    gLast = 0;
}

}  // namespace ScoreView
