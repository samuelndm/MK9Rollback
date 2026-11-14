#pragma once
#include <cstdint>

#include "ArenaPics.h"
#include "Hud.h"

// MK9's own look for the stage screen (docs\feature_parity_plan.md 4.2): the letters of the game's menus and the
// gold frame pieces of its arena select, as ArenaPics read them from the player's installed game. Nothing of it is
// in the mod; where it could not be read the stage screen is drawn plainly (StageScreen.cpp).
//
// Everything is drawn from one texture (ArenaPics' atlas) as one list of triangles: the arena pictures, the frame
// pieces, plain fills (a white block of the atlas) and the lettering. The letters stand in the game's sheets about
// 64 pixels tall; drawn much smaller straight from there they would be ragged, so for each picture size they are
// scaled once on the processor (every pixel the average of the sheet's pixels under it, the baseline on a whole
// pixel) into the atlas' lower half and then drawn pixel for pixel. Three faces: the title, the status line, and
// the small one of the names.
//
// This file makes no Direct3D call and reads no game memory: tests\host\stagescreen_test.cpp draws with it into
// pictures. Colours are multiplied by their alpha, like the atlas' pixels (blend: one, inverse source alpha), so a
// colour with alpha 0 adds light to what lies under it.
namespace StageSkin {

struct Vtx {  // D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};
struct Canvas {  // the vertices being written (a triangle list, six per rectangle); 'dropped' counts rectangles left out
    Vtx* v;
    int n, cap, dropped;
};
enum Face : int { kTitle = 0, kLine, kName, kFaces };  // capitals 44, 27 and 17 units of the frame's 1080

// Rendering thread (in the host test: its one thread). Builds the three faces for this picture size into the art's
// atlas if they are not built for it yet. False: no look for this picture (no art, or the faces do not fit the atlas).
bool Prepare(const ArenaPics::Art* art, const Hud::View& v);
bool ReadyFor(const Hud::View& v);
uint32_t Version();  // changes with every build: the display fills its texture again
void Describe(char* out, int n);  // the last build, for the log
void Reset();        // forget the faces (the host test, between cases)

uint32_t Pm(uint32_t rgb, int alpha);     // a colour at an opacity (0..255)
uint32_t Lit(uint32_t rgb, int strength); // light of a colour added to what lies under it (0..255)
// A rectangle of the picture filled from a rectangle of the atlas, a colour at each corner (top left, top right,
// bottom left, bottom right). 'inset': the atlas' rectangle is cut half a pixel inside its edges, as stretched
// pictures need; letters are drawn without.
void Quad(Canvas& c, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);
void Fill(Canvas& c, int x0, int y0, int x1, int y1, uint32_t colour);
void Fill4(Canvas& c, int x0, int y0, int x1, int y1, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);
void FillV(Canvas& c, int x0, int y0, int x1, int y1, uint32_t top, uint32_t bottom);
void FillH(Canvas& c, int x0, int y0, int x1, int y1, uint32_t left, uint32_t right);
// A frame piece, whole or the columns sx0..sx1 of it, stretched over the rectangle.
void Piece(Canvas& c, ArenaPics::PieceId p, int x0, int y0, int x1, int y1, uint32_t colour, bool mirrored = false);
void PiecePart(Canvas& c, ArenaPics::PieceId p, int sx0, int sx1, int x0, int y0, int x1, int y1, uint32_t colour);
// A round piece (the halo) laid around a box as light: its quarters at the corners, its middle along the edges,
// 'reach' pixels out of the box.
void Glow(Canvas& c, ArenaPics::PieceId p, int x0, int y0, int x1, int y1, int reach, uint32_t colour);
void Picture(Canvas& c, int slot, int x0, int y0, int x1, int y1, uint32_t colour);  // an arena picture of the sheet

int CapOf(Face f);                     // a capital's height in pixels
int Width(Face f, const char* s);
// y: the baseline. Small letters are written as capitals; a sign the faces do not have as '?'. Returns the width.
int Write(Canvas& c, Face f, const char* s, int x, int y, uint32_t colour);

}  // namespace StageSkin
