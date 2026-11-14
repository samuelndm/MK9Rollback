#pragma once
#include <cstdint>

#include "Hud.h"
#include "StageSkin.h"

// The stage screen of an online match (docs\feature_parity_plan.md 4.2): after both players have locked their
// fighters, each picks an arena on this panel, sees the other's pick, and one of the two picks is drawn.
//
// This file is the picture only, drawn with the HUD display's faces and rectangles (Hud.h): a grid of the stages a
// player may pick with RANDOM as its last cell, both players' cursors and picks, a short roulette between the two
// picks, the result, and a two-line banner while the match loads. It reads the State the game thread published and
// nothing else: no game memory, no game calls, no Direct3D. The names are copied into the State when the panel opens
// (net\StageFlow.cpp), so the rendering thread never reads the game's stage table.
// tests\host\stagescreen_test.cpp draws every canned state into pictures without the game.
//
// Three forms of the grid. MK9's look: picture tiles drawn with the game's own menu lettering and the gold frame
// pieces of its arena select (StageSkin.h; like the pictures read from the player's installed game, none of it in the
// mod), where that art could be read. Plain tiles: the game's small arena pictures (ArenaPics.h), 8 across and 4
// down, the name under each in the HUD display's face. Names: a list in three or four columns, where the pictures
// could not be read, are switched off, or the cells are more than the tiles hold. The plain tiles' pictures are
// drawn by Overlay.cpp from the atlas; this file says where (Pictures). MK9's look is one list of textured
// triangles (Look).
namespace StageScreen {

constexpr int kMaxCells = 48;  // the stages a player may pick, and RANDOM
constexpr int kTileCols = 8, kTileCells = 32;  // the tiles' grid: at most 4 rows
constexpr uint8_t kNoCell = 0xFF;
constexpr uint32_t kRouletteMs = 1800;

// kVote: the choice after a match (net\Refight.cpp): the cells are its rows, the cursors and locks the two players'
// choices, 'note' what they gave once that is known. kCard: two lines at the foot of the frame, name[0] and the note
// (a re-fight both chose, on the character select). Both are drawn in MK9's look where its art could be read (the
// same lettering and gold pieces as the grid's), else in the HUD display's faces; never as tiles.
enum Phase : uint8_t { kOff = 0, kPicking, kRoulette, kResult, kBanner, kVote, kCard };
enum Winner : uint8_t { kNoWinner = 0, kMine, kTheirs, kBoth };
// silent: nothing heard from the other panel yet. quiet: it was heard, and then nothing for a second: its cursor is
// drawn faint where it last stood and the status line says "no signal", so a stale cursor is never shown as live.
enum Peer : uint8_t { kPeerSilent = 0, kPeerChoosing, kPeerLocked, kPeerQuiet };
// What became of the draw. The picture never says "arena: X" unless the game was really given X.
enum Use : uint8_t {
    kUsePending = 0,  // not known yet
    kUseApplied,      // the drawn arena is the one the match loads
    kUseGameChose,    // the game's own choice stands ('note' says why: "random" won, no answer in time, ...)
    kUseTrial,        // a trial run: the draw is shown, the game was not given it
};

struct State {  // plain data, no pointers
    uint8_t phase = kOff;
    uint8_t cells = 0;               // names in use; the last one is RANDOM when randomCell says so
    uint8_t randomCell = kNoCell;
    uint8_t myCursor = kNoCell;      // this player's cursor; with myLocked, the pick
    uint8_t theirCursor = kNoCell;   // the other player's cursor; with peer == kPeerLocked, the pick
    uint8_t myLocked = 0;
    uint8_t peer = kPeerSilent;
    uint8_t winner = kNoWinner;      // kRoulette, kResult, kBanner
    uint8_t resultCell = kNoCell;    // the cell the draw named
    uint8_t use = kUsePending;
    uint8_t test = 0;                // the other player is simulated ('stagescreen test'): marked on the panel
    uint8_t tiles = 0;               // picture tiles, where the cells fit them (else the list of names)
    int16_t secondsLeft = -1;        // kPicking: the countdown; -1 none
    uint32_t seed = 0;               // the roulette's length (the same number on both screens)
    uint64_t phaseSinceMs = 0;       // GetTickCount64 when the phase began (the roulette's clock)
    char note[72] = "";              // one more line: the reason of a fallback, a trial run's remark
    char name[kMaxCells][30] = {};
    uint8_t pic[kMaxCells] = {};     // tiles: the cell's picture, its place in ArenaPics' sheet + 1; 0: none (RANDOM)
};

// Game thread.
void Publish(const State& s);
// The rendering thread has drawn within the last second with faces built for the picture: a panel would be seen.
bool CanDraw();

// Rendering thread (Overlay.cpp). faces: Hud::Prepare's answer for this picture. Returns the vertices written (a
// triangle list), 0 when nothing is shown.
int Build(const Hud::View& v, bool faces, uint64_t nowMs, Hud::Vtx* out, int cap);
// One state, as given (the host test; Build uses it).
int BuildState(const State& s, const Hud::View& v, uint64_t nowMs, Hud::Vtx* out, int cap);
int Dropped();  // rectangles left out of the last BuildState because the buffer was full
// The pictures of the last Build / BuildState: where each goes, and the vertex before which they are drawn (the
// rectangles before it lie under the pictures, those after it over them). Returns their number, 0 for the list.
struct Pic {
    int x0, y0, x1, y1;
    uint8_t slot;  // the picture's place in ArenaPics' sheet
};
int Pictures(const Pic** list, int* split);
// MK9's look: the textured triangles of the last Build / BuildState (StageSkin::Vtx, drawn from ArenaPics' atlas after
// the plain rectangles, which then are only the dimming of the game's picture). 0: a plain form was drawn.
int Look(const StageSkin::Vtx** v);
void Counters(long* draws, long* largest, long* dropped);
void LookCounters(long* draws, long* largest);

// 'stagedemo N': canned state N instead of the published one (0: off). Frozen in time, for screenshots.
constexpr int kDemos = 13;      // the arena panel's
constexpr int kVoteDemos = 5;   // numbers kDemos + 1 ...: the choice after a match (four states) and the re-fight's card
bool Demo(int n, uint64_t nowMs, State* s);
void SetDemo(int n);
int DemoShown();

// ---- pure helpers (also used by the flow and the host test) ----
// A readable name for a stage table entry: "BGND_STGardensN" -> "SHANG TSUNG GARDENS NIGHT". Made from the table's own
// text by rule (words split at capitals, ST / SK spelled out, a final N is "night"); not the game's menu names.
void DisplayName(const char* tableName, char* out, int cap);
// Tiles are drawn: they are wanted and the cells fit them.
bool Tiles(int cells, bool wanted);
// The grid for a number of cells. Names: 3 columns up to 30 cells, 4 above (the smaller face). Tiles: 8 columns,
// each cell a picture 16:9 (the game's pictures are 16:9 squeezed into a square) and room for two lines of its name.
struct Box {
    int x0, y0, x1, y1;
};
struct Layout {
    int cols = 0, rows = 0;
    bool tiles = false;
    Hud::FaceId face = Hud::kMainFace;
    Box frame{}, panel{}, cell[kMaxCells] = {};
    Box pic[kMaxCells] = {};  // tiles: the picture's place in its cell
};
Layout LayoutOf(int cells, bool tiles, const Hud::View& v);
// The cursor after a step (dx, dy: -1, 0, 1), wrapping at the edges and skipping the holes of a short last row.
uint8_t Move(int cells, bool tiles, uint8_t from, int dx, int dy);
// A name on at most two lines of a width: broken at the space that leaves the two lines most alike, and cut where
// even that does not fit. Returns the number of lines (1 or 2).
int Wrap(Hud::FaceId f, const char* text, int maxW, char* a, char* b, int cap);
// The same in the small face of MK9's look, and the width a name has under its tile there.
int WrapLook(const char* text, int maxW, char* a, char* b, int cap);
int LookNameRoom(const Layout& l, const Hud::View& v);
// The roulette: which of the two picks is lit at this moment (0 this player's, 1 the other's). It ends on the winner.
int RouletteLit(const State& s, uint64_t nowMs);

}  // namespace StageScreen
