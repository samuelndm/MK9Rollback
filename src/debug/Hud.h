#pragma once
#include <cstdint>

#include "Overlay.h"

// The network status drawn as part of the fight's HUD (docs\feature_parity_plan.md 4.1): a small group under the round
// timer (connection bars, round trip in ms, frames rolled back, input delay), a Wi-Fi mark at each player's side, short
// notes under the group, and a badge before the fight. Text with a dark edge, no box.
//
// This file only turns the published numbers into coloured rectangles in back-buffer pixels. It makes no Direct3D call
// and reads no game memory, so tests\host\hud_test.cpp runs it without the game and draws the result into pictures.
// Overlay.cpp hands the rectangles to the device, on the rendering thread, as it did for the old status line.
//
// Text: Windows renders each glyph once per resolution, antialiased, at the size the picture needs
// (GetGlyphOutline's 65 coverage levels, kept as 16). The glyph's coverage is stored as rectangles of equal value and
// drawn blended: no texture, nothing created on the device. The dark edge is a second set of rectangles, the coverage
// widened by one to two pixels.
namespace Hud {

struct Vtx {  // D3DFVF_XYZRHW | D3DFVF_DIFFUSE
    float x, y, z, rhw;
    uint32_t color;
};

struct View {  // the back buffer's viewport, in pixels
    int x = 0, y = 0, w = 0, h = 0;
};

// Rendering thread. Builds the two faces for this picture size if they are not built for it yet (10-20 ms, once per
// resolution). False: no faces for this size. Either Windows gave no usable face, and the caller draws the old line
// instead; or the size changed again within two seconds of the last build and this one waits (*wait is set), and the
// caller draws nothing for now.
bool Prepare(const View& v, bool* wait = nullptr);
// How many times Prepare has built faces (the caller logs each one).
int Builds();

// Rendering thread. The frame's triangles (a triangle list, six vertices per rectangle) for the stats as published,
// with fades advanced to nowMs. Returns the number of vertices written, 0 when nothing is on screen.
int Build(const Overlay::Stats& s, const View& v, uint64_t nowMs, Vtx* out, int cap);

// Rectangles left out of the last Build because the buffer was full (0 in every tested case).
int Dropped();

// What Prepare built: the face Windows gave and the two pixel sizes. For the log.
void Describe(char* out, int n);

// Connection quality from the network inputs only (not the mode): 0 good, 1 so-so, 2 bad. The bars' colour and count.
int Quality(const Overlay::Stats& s);
// Short player text for a raw stop reason (GgpoStop / Disengage); anything unknown is returned as it is.
const char* StopText(const char* why);

// Canned stats for a look at each state without an online match ('hud demo N', and the host test). False: no such N.
constexpr int kDemos = 12;
bool Demo(int n, Overlay::Stats* s);

// Forget the fades and the faces (the host test, between cases).
void Reset();

}  // namespace Hud
