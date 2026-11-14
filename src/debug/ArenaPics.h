#pragma once
#include <cstdint>

// The arena pictures of the stage screen (docs\feature_parity_plan.md 4.2): the small pictures MK9's own arena select
// shows, read at run time from the player's installed game (Asset\ui_ps_arenaSelect.xxx beside MKKE.exe). Nothing of
// the game's art is in this repository or in a release; a PC whose file is missing or different gets the names only.
//
// The file is an Unreal Engine 3 package as MK9 cooks it (read from the file itself on 2026-11-12, tools\xxx_look.ps1;
// not from documentation): file version 472, licensee 46, everything after the summary in chunks of LZO1X blocks.
// Each picture is a Texture2D named thumb_<arena>, 128 x 128, DXT5 (DXT1 is read too), one size, squeezed from 16:9.
//
// The same file holds what MK9's arena select is drawn with, and the reader takes that too (the "art"): two of the
// menus' fonts as sheets of letters ("Birch Std MKLargeMenu", "Birch Std MKSmallMenu", 1024 x 512, DXT5 with alpha),
// six frame pieces (ui_ps_arenaSelect_I..: a gold bar with corner ornaments, a line with a sun, a ring, three glows),
// and the Scaleform movie (class MwyUIMovieData, "GFX" version 8), whose font records say where each letter stands
// in its sheet, how far the pen moves after it and which pairs of letters are moved together. With the art the stage
// screen is drawn in MK9's own look (StageSkin.h); without it, as plain tiles or as the list of names.
//
// The reader is pure (no game memory, no Direct3D): tests\host\arenapics_test.cpp runs it on a package it writes
// itself, on damaged copies of that package, and on the installed file when there is one. The mod's own set is read
// once, on a thread of its own at low priority, never on the game's or the rendering thread.
namespace ArenaPics {

constexpr int kSlots = 32, kSide = 128;           // the pictures stand in one sheet, 8 across and 4 down
constexpr int kSheetW = 1024, kSheetH = 512;
constexpr int kNameLen = 24;
// The mod's one texture. Its upper left quarter is the sheet of pictures, the upper right holds the frame pieces
// (PlaceOf), the lower half the lettering at the sizes the picture on screen needs (StageSkin.cpp).
constexpr int kAtlasW = 2048, kAtlasH = 1024;

// ---- pure ----
// LZO1X, with every read and write checked. Returns the bytes written; -1 for a stream that is damaged, runs past
// its end or does not fit dst.
int LzoDecompress(const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t dstCap);

// The picture's name for a stage table entry: "BGND_STGardensN" -> "stgardensn" (small letters without "BGND_"; three
// arenas have another name in the package: KahnsColiseum koliseum, STThrone shangsthroneroom, STGardens stgardenday).
void PictureName(const char* tableName, char* out, int cap);

struct Source {  // the file: read n bytes at an offset
    bool (*read)(void* ctx, uint64_t at, void* buf, uint32_t n);
    void* ctx;
    uint64_t size;
};
struct Set {
    int count = 0;                    // pictures read; picture i stands at ((i % 8) * 128, (i / 8) * 128) of the sheet
    char name[kSlots][kNameLen] = {}; // "pit", "stgardensn", ...
    uint32_t* sheet = nullptr;        // kSheetH rows of pixels 0xFFRRGGBB, given by the caller
    int stride = kSheetW;             // pixels from one row to the next (kAtlasW when the sheet is an atlas' corner)
    // what the load did (the log line)
    int version = 0, licensee = 0, chunks = 0, exports = 0, textures = 0, skipped = 0, chunksRead = 0;
    uint32_t fileBytes = 0, unpackedBytes = 0;
    char why[120] = "";               // empty, or why nothing (or not everything) was read
};

// ---- the game's lettering and frame pieces ----
constexpr int kFontW = 1024, kFontH = 512;
constexpr int kFirstCode = 0x20, kCodes = 64;  // space to underscore: capitals, digits, punctuation
enum FontId : int { kLargeMenu = 0, kSmallMenu, kFonts };
enum PieceId : int {
    kBar = 0,  // I2F 512 x 64: a gold line, a Greek-key ornament at each end
    kSun,      // I3B 512 x 64: a thin gold line that fades at its ends, a sun hanging from its middle
    kGlow,     // I45 128 x 64: an orange glow, brightest at its right edge
    kHalo,     // I51 64 x 64: a soft white disc
    kRing,     // I38 256 x 256: a gold ring
    kRed,      // I16 512 x 512, kept at a quarter of its size: a soft red glow
    kWhite,    // not from the file: a white block, for plain fills in the same draw call
    kPieces
};
struct Place {
    uint16_t x, y, w, h;
};
Place PlaceOf(PieceId p);  // where a piece stands in the atlas
struct Glyph {             // one letter in its font's sheet
    uint16_t x = 0, y = 0, w = 0, h = 0;  // its box (w 0: nothing to draw, a space)
    float ox = 0, oy = 0;                 // the pen in the box: from its left edge, and from its top down to the baseline
    float advance = 0;                    // how far the pen moves, in EMs
};
struct Font {
    uint32_t* px = nullptr;             // kFontW x kFontH, 0xAARRGGBB, colours multiplied by alpha; given by the caller
    Glyph glyph[kCodes];
    int16_t kern[kCodes][kCodes] = {};  // [left letter][right letter]: the pen's correction, in 1/20480 EM
    float em = 0, cap = 0;              // sheet pixels per EM, and of a capital letter's height
    int glyphs = 0, pairs = 0;          // letters with a picture, kerning pairs taken (the log line)
};
struct Art {
    uint32_t* atlas = nullptr;  // kAtlasW x kAtlasH, colours multiplied by alpha; given by the caller. Load writes the pieces.
    Font font[kFonts];
    bool ok = false;
    char why[120] = "";         // why there is no art
};

// Reads the package's thumb_* pictures into out->sheet. False: no picture could be read (out->why says why).
// With 'art': its lettering and frame pieces too (art->ok; art->why says why not). The pictures do not depend on it.
bool Load(const Source& src, Set* out, Art* art = nullptr);
bool LoadFile(const wchar_t* path, Set* out, Art* art = nullptr);
int Find(const Set& s, const char* pictureName);  // -1: none

// ---- the mod's one set ----
enum Status : int { kNotAsked = 0, kLoading, kReady, kFailed };
void Request();     // any thread: the first call starts the reading thread; later calls do nothing
int State();
// Ready, and neither switched off nor refused by the rendering thread: the stage screen may show picture tiles.
bool Usable();
int Count();
int SlotOf(const char* tableName);  // the picture's place in the sheet for a stage table name, -1 without one
int SlotOfPicture(const char* pictureName);  // the same for a picture's own name (PictureName's answer)
// The atlas' pixels (kAtlasW x kAtlasH), once ready. Its upper half never changes afterwards; its lower half is
// the rendering thread's (StageSkin::Prepare).
uint32_t* Atlas();
// The art, once ready and where it could be read whole and the look is not switched off: nullptr otherwise.
const Art* ArtOf();
void SetWanted(bool on);            // 'stagepics 0|1'
void SetLookWanted(bool on);        // 'stagelook 0|1': MK9's look, or the plain tiles
void Refuse(const char* why);       // the rendering thread could not make its texture: names only from here on
void Describe(char* out, int n);    // one line for the log
void Shutdown();                    // waits for the reading thread and gives its memory back (the DLL is about to unload)
// The host test: this set (its sheet must stay alive, an atlas' corner where art is given) is the mod's, or none.
void Adopt(const Set* s, const Art* art = nullptr);

}  // namespace ArenaPics
