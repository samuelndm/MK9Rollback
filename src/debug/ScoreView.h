#pragma once
#include <cstdint>

#include "Hud.h"

// The score in a fight (the user's wish of 2026-11-16: "show it better ... on the fighting screen as well! Also show the
// win streak if possible!"): under each health bar, beside the portrait, that player's wins in this room (against this
// opponent since the two met in this game, until one leaves: net\ScoreKeeper.h) in large numbers, and under them the
// wins in a row of the one who is on a run ("3 IN A ROW").
//
// Drawn with the HUD display's faces (Hud.h) as plain rectangles: no texture is made or kept in a fight. The game
// thread (net\ScoreKeeper.cpp) publishes the numbers while an online one-on-one fight runs (state 8: not in a
// finisher, where the game takes its own HUD away); this file only turns them into rectangles and fades them in and
// out. It reads no game memory and makes no Direct3D call: tests\host\scoreview_test.cpp draws it into pictures.
namespace ScoreView {

struct Badge {  // plain data
    uint8_t on = 0;
    uint8_t mySide = 0;          // the fighter this PC plays, 1 or 2 (for the log and the demos; both sides look alike)
    uint16_t wins[2] = {0, 0};   // the room's wins: [0] player 1's (the left health bar), [1] player 2's
    uint16_t run[2] = {0, 0};    // the wins in a row each player is on (at most one of the two is not 0)
};

// Game thread: what to show now (an equal one changes nothing).
void Publish(const Badge& b);

// Rendering thread (Overlay.cpp), after Hud::Prepare: the published badge with its fade at nowMs. Returns the vertices
// written (a triangle list), 0 when nothing is shown.
int Build(const Hud::View& v, bool faces, uint64_t nowMs, Hud::Vtx* out, int cap);
// One badge as given, at full strength (the host test).
int BuildBadge(const Badge& b, const Hud::View& v, int alpha, Hud::Vtx* out, int cap);
int Dropped();  // rectangles left out of the last build because the buffer was full

// 'scoredemo N': a canned badge in place of the published one, wherever the game is (0: off). For screenshots.
constexpr int kDemos = 4;
bool Demo(int n, Badge* b);
void SetDemo(int n);
int DemoShown();
void Reset();  // the fade (the host test, between cases)

}  // namespace ScoreView
