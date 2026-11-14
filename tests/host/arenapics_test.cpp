// Host test of src\debug\ArenaPics.cpp: the reader of the stage screen's arena pictures, without the game.
//   arenapics_test.exe <out folder> [the installed game's ui_ps_arenaSelect.xxx]
// 1. LZO1X: hand-made streams with matches, streams of literals of every length class, every cut-off stream and
//    100000 streams of nonsense, with the input ending and the output ending at a page without access (a read or a
//    write past either end stops the test).
// 2. A package this test writes itself in MK9's layout (summary, chunk table, chunks of LZO blocks, name / import /
//    export tables, Texture2D objects with known pixels): what is read, what is left out, the exact pixels and their
//    places in the sheet; the same package unpacked; one with more pictures than the sheet holds.
// 3. Thousands of damaged copies of that package: every one is read to an end, nothing is written outside the sheet.
// 3a. MK9's lettering and frame pieces: a package with a movie this test writes itself (fonts as the exporter stores
//    them, their textures' tables), two sheets of letters and the six pieces with known pixels and alpha: every
//    letter's box, pen, advance and kerning, every pixel of the pieces at its place in the atlas; each form the reader
//    does not expect is refused with its reason while the pictures are read all the same; damaged copies.
// 4. The names: stage table entry -> picture.
// 5. The mod's own set: the reading thread where the game's file is missing (names only), a set taken over, the
//    switches, a refusal by the rendering thread.
// 6. With the installed game's file: its pictures (count, one for each of the 28 arenas a player may pick, how much
//    of the file was read, the time) and its lettering and frame pieces. The sheet and the atlas are written as
//    pictures beside the test's other output; they are the game's art and stay out of the repository and of releases
//    (build\ is not tracked).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/debug/ArenaPics.h"

namespace {
using namespace ArenaPics;

int gChecks = 0, gFailed = 0;
void Check(bool ok, const char* what, const char* detail = "") {
    ++gChecks;
    if (!ok) {
        ++gFailed;
        printf("FAIL %s %s\n", what, detail);
    }
}

struct Rng {
    uint32_t s;
    uint32_t Next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
};

// Memory that ends where a page without access begins: Tail(n) is its last n bytes.
struct Guarded {
    uint8_t* end = nullptr;
    explicit Guarded(size_t cap) {
        const size_t page = 4096, body = (cap + page - 1) / page * page;
        uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, body + page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!base) {
            printf("FAIL no memory for a guarded buffer\n");
            exit(2);
        }
        DWORD old;
        VirtualProtect(base + body, page, PAGE_NOACCESS, &old);
        end = base + body;
    }
    uint8_t* Tail(size_t n) const { return end - n; }
};
// The sheet between two pages without access.
uint32_t* GuardedSheet() {
    const size_t page = 4096, body = (size_t)kSheetW * kSheetH * 4;
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, body + 2 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!base) {
        printf("FAIL no memory for the sheet\n");
        exit(2);
    }
    DWORD old;
    VirtualProtect(base, page, PAGE_NOACCESS, &old);
    VirtualProtect(base + page + body, page, PAGE_NOACCESS, &old);
    return reinterpret_cast<uint32_t*>(base + page);
}

// An LZO1X stream that holds its bytes as literals: one run and the end mark.
void Stored(const uint8_t* d, uint32_t n, std::vector<uint8_t>& out) {
    if (n > 0 && n <= 238) {
        out.push_back((uint8_t)(17 + n));
    } else if (n > 238) {
        const uint32_t rem = n - 18, zeros = (rem - 1) / 255;  // n = 18 + 255 * zeros + last, last 1..255
        out.push_back(0);
        out.insert(out.end(), zeros, 0);
        out.push_back((uint8_t)(rem - zeros * 255));
    }
    out.insert(out.end(), d, d + n);
    out.push_back(0x11), out.push_back(0), out.push_back(0);
}

// A whole LZO1X stream of random instructions of every kind (runs of literals short and long, matches near, middle
// and far, the short matches after literals, one to three literals after a match), and what it unpacks to, kept by
// this writer's own copying.
void RandomStream(Rng& rng, std::vector<uint8_t>& s, std::vector<uint8_t>& out, int steps) {
    enum { kPlain, kAfterRun, kAfterMatchLiterals } state = kPlain;
    const auto literals = [&](uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t b = (uint8_t)(rng.Below(3) ? 'a' + rng.Below(4) : rng.Next());
            s.push_back(b), out.push_back(b);
        }
    };
    const auto copy = [&](uint32_t distance, uint32_t len) {
        for (uint32_t i = 0; i < len; ++i) out.push_back(out[out.size() - distance]);
    };
    const auto longer = [&](uint32_t more) {  // a length that did not fit its instruction: zeros, then the rest
        for (; more > 255; more -= 255) s.push_back(0);
        s.push_back((uint8_t)more);
    };
    const uint32_t first = rng.Below(3) ? 1 + rng.Below(60) : 0;  // the stream may begin with a counted run
    if (first) {
        s.push_back((uint8_t)(17 + first));
        literals(first);
        state = first < 4 ? kAfterMatchLiterals : kAfterRun;
    }
    for (int k = 0; k < steps; ++k) {
        const uint32_t after = rng.Below(4);  // literals after a match
        const uint32_t have = (uint32_t)out.size();
        const uint32_t kind = rng.Below(10);
        if (state == kPlain && (kind < 3 || have == 0)) {  // a run of literals
            const uint32_t n = rng.Below(8) ? 4 + rng.Below(15) : 19 + rng.Below(700);
            if (n <= 18) {
                s.push_back((uint8_t)(n - 3));
            } else {
                s.push_back(0);
                longer(n - 18);
            }
            literals(n);
            state = kAfterRun;
            continue;
        }
        if (state == kAfterRun && kind < 2 && have >= 0x801) {  // three bytes from at least 2049 back
            const uint32_t d = rng.Below(std::min<uint32_t>(have - 0x800, 1024));  // distance 0x801 + d
            s.push_back((uint8_t)((d & 3) << 2 | after)), s.push_back((uint8_t)(d >> 2));
            copy(0x801 + d, 3);
        } else if (state == kAfterMatchLiterals && kind < 2 && have >= 1) {  // two bytes from at most 1024 back
            const uint32_t d = rng.Below(std::min<uint32_t>(have, 1024));
            s.push_back((uint8_t)((d & 3) << 2 | after)), s.push_back((uint8_t)(d >> 2));
            copy(1 + d, 2);
        } else if (kind < 6) {  // near: 3 to 8 bytes from at most 2048 back
            const uint32_t d = rng.Below(std::min<uint32_t>(have, 2048)), len = 3 + rng.Below(6);
            s.push_back((uint8_t)((len - 1) << 5 | (d & 7) << 2 | after)), s.push_back((uint8_t)(d >> 3));
            copy(1 + d, len);
        } else if (kind < 9 || have < 0x4001) {  // middle: any length from at most 16384 back
            const uint32_t d = rng.Below(std::min<uint32_t>(have, 16384)), len = rng.Below(6) ? 3 + rng.Below(31) : 34 + rng.Below(900);
            if (len - 2 <= 31) {
                s.push_back((uint8_t)(32 | (len - 2)));
            } else {
                s.push_back(32);
                longer(len - 2 - 31);
            }
            s.push_back((uint8_t)((d & 63) << 2 | after)), s.push_back((uint8_t)(d >> 6));
            copy(1 + d, len);
        } else {  // far: from 16385 to 49151 back
            const uint32_t d = 1 + rng.Below(std::min<uint32_t>(have - 0x4000, 0x7FFF)), len = rng.Below(6) ? 3 + rng.Below(7) : 10 + rng.Below(600);
            const uint32_t high = (d >> 14) & 1, low = d & 0x3FFF;
            if (len - 2 <= 7) {
                s.push_back((uint8_t)(16 | high << 3 | (len - 2)));
            } else {
                s.push_back((uint8_t)(16 | high << 3));
                longer(len - 2 - 7);
            }
            s.push_back((uint8_t)((low & 63) << 2 | after)), s.push_back((uint8_t)(low >> 6));
            copy(0x4000 + d, len);
        }
        literals(after);
        state = after ? kAfterMatchLiterals : kPlain;
    }
    s.push_back(0x11), s.push_back(0), s.push_back(0);
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i)));
}
void Set32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = (uint8_t)(x >> (8 * i));
}

// ---- a package in MK9's layout ----
struct Thing {
    std::string name;
    int number = 0;        // the name's number (0: none)
    bool texture = true;   // its class is Texture2D
    int side = 128;        // width and height
    int format = 7;        // 7 DXT5, 5 DXT1
    uint32_t flags = 0;    // of the picture's data
    int seed = 0;          // colours
    bool marked = false;   // its first block holds four different colours
    uint32_t filler = 0;   // not a texture: this many bytes
    int height = 0;        // a texture that is not square: its height (0: as high as wide)
    bool art = false;      // a texture with alpha, as the lettering and the frame pieces have it (ArtAlpha, ExpectedArt)
    bool movie = false;    // not a texture: the movie (class MwyUIMovieData), its bytes in 'raw'
    std::vector<uint8_t> raw;
    uint32_t at = 0;
    std::vector<uint8_t> bytes;
};
struct Pack {
    std::vector<std::string> names{"None", "Core", "Class", "\x01"};  // "\x01": a name in two-byte characters
    std::vector<Thing> things;
    bool withMovie = false;     // a third import, the movie's class (the packages without it stay byte for byte what they were)
    bool plain = false;         // not packed: no chunks
    uint32_t chunkBytes = 150000;
    int Name(const char* s) {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == s) return (int)i;
        names.push_back(s);
        return (int)names.size() - 1;
    }
};

uint32_t Colour565(int seed, int bx, int by) { return (uint32_t)((bx & 31) << 11 | ((by * 2) & 63) << 5 | (seed & 31)); }
uint32_t Expand565(uint32_t c) {
    const uint32_t r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    return 0xFF000000u | r << 16 | g << 8 | b;
}
// What the reader must give for a pixel of a Thing's picture.
uint32_t ExpectedPixel(const Thing& t, int x, int y) {
    if (t.marked && x < 4 && y < 4) {
        static const uint32_t dxt5[4] = {0xFFFFFFFFu, 0xFF000000u, 0xFFAAAAAAu, 0xFF555555u};  // white, black, 2:1, 1:2
        static const uint32_t dxt1[4] = {0xFF000000u, 0xFFFFFFFFu, 0xFF7F7F7Fu, 0xFF000000u};  // black, white, the middle, black
        return (t.format == 5 ? dxt1 : dxt5)[x];
    }
    return Expand565(Colour565(t.seed, x / 4, y / 4));
}

// An art texture's alpha: by block, opaque, clear, or the first of the six steps between (6 * 255 / 7 = 218).
int ArtAlpha(int bx, int by) { return (bx + by) % 3 == 0 ? 255 : (bx + by) % 3 == 1 ? 0 : 218; }
// What the reader must give for a pixel of an art texture: the block's colour multiplied by its alpha.
uint32_t ExpectedArt(const Thing& t, int x, int y) {
    const uint32_t c = Expand565(Colour565(t.seed, x / 4, y / 4));
    const uint32_t a = (uint32_t)ArtAlpha(x / 4, y / 4);
    return a << 24 | (((c >> 16) & 255) * a + 127) / 255 << 16 | (((c >> 8) & 255) * a + 127) / 255 << 8 | ((c & 255) * a + 127) / 255;
}

void PutName(std::vector<uint8_t>& v, int index, int number = 0) {
    Put32(v, (uint32_t)index);
    Put32(v, (uint32_t)number);
}
void PutProp(Pack& p, std::vector<uint8_t>& v, const char* name, const char* type, uint32_t size) {
    PutName(v, p.Name(name));
    PutName(v, p.Name(type));
    Put32(v, size);
    Put32(v, 0);  // array index
}

// A Texture2D as the game's file holds one (see ArenaPics.cpp): 'at' is where the object will stand unpacked.
void MakeTexture(Pack& p, Thing& t, Rng& rng) {
    std::vector<uint8_t>& v = t.bytes;
    v.clear();
    PutProp(p, v, "SizeX", "IntProperty", 4), Put32(v, (uint32_t)t.side);
    PutProp(p, v, "SizeY", "IntProperty", 4), Put32(v, (uint32_t)t.side);
    PutProp(p, v, "Tint", "StructProperty", 4), PutName(v, p.Name("Color")), Put32(v, 0x11223344);
    PutProp(p, v, "Format", "ByteProperty", 1), v.push_back((uint8_t)t.format);
    PutProp(p, v, "NeverStream", "BoolProperty", 0), Put32(v, 1);
    PutProp(p, v, "LODGroup", "ByteProperty", 1), v.push_back(21);
    PutName(v, p.Name("None"));
    for (int i = 0; i < 2; ++i) {  // two empty data headers
        Put32(v, 0), Put32(v, 0), Put32(v, 0);
        Put32(v, t.at + (uint32_t)v.size() + 4);
    }
    Put32(v, 1);  // one size
    const bool dxt1 = t.format == 5;
    const int high = t.height ? t.height : t.side;
    const uint32_t bytes = (uint32_t)(t.side / 4) * (uint32_t)(high / 4) * (dxt1 ? 8 : 16);
    Put32(v, t.flags), Put32(v, bytes), Put32(v, bytes);
    Put32(v, t.at + (uint32_t)v.size() + 4);
    for (int by = 0; by < high / 4; ++by)
        for (int bx = 0; bx < t.side / 4; ++bx) {
            if (!dxt1 && t.art) {  // alphas 255 and 0; every pixel takes the first, the second, or the first step between
                const int a = ArtAlpha(bx, by);
                const uint64_t pick = a == 255 ? 0 : a == 0 ? 1 : 2;
                uint64_t bits = 0;
                for (int i = 0; i < 16; ++i) bits |= pick << (3 * i);
                v.push_back(255), v.push_back(0);
                for (int i = 0; i < 6; ++i) v.push_back((uint8_t)(bits >> (8 * i)));
            } else if (!dxt1)
                for (int i = 0; i < 8; ++i) v.push_back((uint8_t)rng.Next());  // the alpha block: anything
            uint32_t c0 = Colour565(t.seed, bx, by), c1 = c0, idx = rng.Next() << 8 | (rng.Next() & 255);
            // Two equal colours: every index gives that colour in DXT5; in DXT1 (three colours and black) index 3 is black.
            if (dxt1) idx &= 0x55555555u | ((~idx & 0x55555555u) << 1);
            if (t.marked && bx == 0 && by == 0) {
                c0 = dxt1 ? 0x0000 : 0xFFFF, c1 = dxt1 ? 0xFFFF : 0x0000;
                idx = 0xE4E4E4E4u;  // pixel i takes colour i & 3
            }
            v.push_back((uint8_t)c0), v.push_back((uint8_t)(c0 >> 8)), v.push_back((uint8_t)c1), v.push_back((uint8_t)(c1 >> 8));
            Put32(v, idx);
        }
    Put32(v, (uint32_t)t.side), Put32(v, (uint32_t)high);
}

// The file. Tables first, then the objects; the unpacked package cut into chunks of chunkBytes (so that tables and
// objects cross chunk and block borders), every block held as LZO literals.
std::vector<uint8_t> BuildFile(Pack& p, uint32_t seed) {
    Rng rng{seed};
    const int texture = p.Name("Texture2D"), core = p.Name("Core"), cls = p.Name("Class"), package = p.Name("Package");
    // The names every texture uses must exist before the name table is laid out: make one to collect them.
    {
        Thing probe;
        MakeTexture(p, probe, rng);
    }
    for (Thing& t : p.things) p.Name(t.name.c_str());
    const int movieClass = p.withMovie ? p.Name("MwyUIMovieData") : -1;
    const uint32_t summary = p.plain ? 97u : 171u;  // the unpacked package's summary: tables start after it
    std::vector<uint8_t> u(summary, 0);
    const uint32_t nameOff = (uint32_t)u.size();
    for (size_t i = 0; i < p.names.size(); ++i) {
        if (p.names[i] == "\x01") {  // the reader steps over it
            Put32(u, (uint32_t)-3);
            for (const char ch : {'x', 'y', '\0'}) u.push_back((uint8_t)ch), u.push_back(0);
            Put32(u, 0), Put32(u, 0);
            continue;
        }
        Put32(u, (uint32_t)p.names[i].size() + 1);
        u.insert(u.end(), p.names[i].begin(), p.names[i].end());
        u.push_back(0);
        Put32(u, 0x00070010), Put32(u, 0);
    }
    const int nameCount = (int)p.names.size();
    const uint32_t importOff = (uint32_t)u.size();
    PutName(u, core), PutName(u, cls), Put32(u, 0), PutName(u, texture);  // import 0: Texture2D
    PutName(u, core), PutName(u, cls), Put32(u, 0), PutName(u, package);  // import 1: something else
    if (p.withMovie) PutName(u, core), PutName(u, cls), Put32(u, 0), PutName(u, movieClass);  // import 2: the movie's class
    const uint32_t exportOff = (uint32_t)u.size();
    uint32_t at = exportOff + (uint32_t)p.things.size() * 64;
    for (Thing& t : p.things) {
        t.at = at;
        if (t.movie) {
            t.bytes = t.raw;
        } else if (t.texture) {
            MakeTexture(p, t, rng);
        } else {
            t.bytes.resize(t.filler);
            for (uint8_t& b : t.bytes) b = (uint8_t)rng.Next();
        }
        at += (uint32_t)t.bytes.size();
    }
    for (const Thing& t : p.things) {
        Put32(u, t.movie ? (uint32_t)-3 : t.texture ? (uint32_t)-1 : (uint32_t)-2);  // class: import 2, 0 or 1
        Put32(u, 0), Put32(u, 0);
        PutName(u, p.Name(t.name.c_str()), t.number);
        Put32(u, 0);
        Put32(u, 0x000F0004), Put32(u, 0);
        Put32(u, (uint32_t)t.bytes.size()), Put32(u, t.at);
        for (int i = 0; i < 6; ++i) Put32(u, 0);
    }
    for (const Thing& t : p.things) u.insert(u.end(), t.bytes.begin(), t.bytes.end());
    if (nameCount != (int)p.names.size()) printf("FAIL the test's package: a name was added after the name table was written\n"), exit(2);

    // the file's summary
    std::vector<uint8_t> f;
    Put32(f, 0x9E2A83C1u);
    f.push_back(472 & 255), f.push_back(472 >> 8), f.push_back(46), f.push_back(0);
    Put32(f, 0);  // header size, filled below
    f.push_back('M'), f.push_back('K'), f.push_back(' '), f.push_back(' ');
    Put32(f, 62);
    Put32(f, 5), f.insert(f.end(), {'N', 'o', 'n', 'e', 0});
    Put32(f, 0x02880009);
    Put32(f, (uint32_t)nameCount), Put32(f, nameOff);
    Put32(f, (uint32_t)p.things.size()), Put32(f, exportOff);
    Put32(f, p.withMovie ? 3 : 2), Put32(f, importOff);
    Put32(f, (uint32_t)u.size());  // depends
    Put32(f, 0);
    for (int i = 0; i < 16; ++i) f.push_back((uint8_t)(0xA0 + i));
    Put32(f, 2605), Put32(f, 139);
    if (p.plain) {
        Put32(f, 0), Put32(f, 0);
        Set32(f, 8, (uint32_t)f.size());
        memcpy(u.data(), f.data(), f.size());  // 97 bytes: the file is the package
        return u;
    }
    struct Cut {
        uint32_t uAt, uSize;
        std::vector<uint8_t> packed;
    };
    std::vector<Cut> cuts;
    for (uint32_t from = summary; from < u.size();) {
        Cut c;
        c.uAt = from;
        c.uSize = std::min<uint32_t>(p.chunkBytes, (uint32_t)u.size() - from);
        const uint32_t block = 0x20000, blocks = (c.uSize + block - 1) / block;
        std::vector<uint8_t> data, table;
        for (uint32_t b = 0; b < blocks; ++b) {
            const uint32_t us = std::min(block, c.uSize - b * block);
            const size_t before = data.size();
            Stored(u.data() + from + b * block, us, data);
            Put32(table, (uint32_t)(data.size() - before)), Put32(table, us);
        }
        Put32(c.packed, 0x9E2A83C1u), Put32(c.packed, block), Put32(c.packed, (uint32_t)data.size()), Put32(c.packed, c.uSize);
        c.packed.insert(c.packed.end(), table.begin(), table.end());
        c.packed.insert(c.packed.end(), data.begin(), data.end());
        cuts.push_back(std::move(c));
        from += cuts.back().uSize;
    }
    Put32(f, 2), Put32(f, (uint32_t)cuts.size());
    const size_t tableAt = f.size();
    f.resize(f.size() + cuts.size() * 16 + 74, 0x5A);  // the table, then what else MK9's summary holds
    Set32(f, 8, (uint32_t)f.size());
    for (size_t i = 0; i < cuts.size(); ++i) {
        Set32(f, tableAt + i * 16, cuts[i].uAt), Set32(f, tableAt + i * 16 + 4, cuts[i].uSize);
        Set32(f, tableAt + i * 16 + 8, (uint32_t)f.size()), Set32(f, tableAt + i * 16 + 12, (uint32_t)cuts[i].packed.size());
        f.insert(f.end(), cuts[i].packed.begin(), cuts[i].packed.end());
    }
    return f;
}

// ---- a movie as MK9's exporter writes it (see ArenaPics.cpp, ReadFonts) ----
void Put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x), v.push_back((uint8_t)(x >> 8)); }
void PutF(std::vector<uint8_t>& v, float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    Put32(v, x);
}
constexpr int kTestGlyphs = 70, kTestInTexture = 66, kTestPad = 6, kTestNominal = 127;
// Glyph i of a test font: its code (0x20 upwards; the last ones lie outside what the reader takes, and one repeats
// 'A'), its advance, and its place in the texture's table (the space and the last three have none).
int TestCode(int i) { return i < 64 ? 0x20 + i : i == 64 ? 'A' : 0x100 + i; }
int TestAdvance(int i) { return 2000 + 37 * i; }
int TestInTexture(int i) { return i >= 1 && i <= kTestInTexture ? kTestInTexture - i : -1; }
struct TestBox {
    int x, y, w, h;
    float ox, oy;
};
TestBox TestBoxOf(int inTexture) { return TestBox{inTexture % 16 * 64 + 3, inTexture / 16 * 100 + 2, 20 + inTexture % 30, 72 + inTexture % 20, 5.75f, 69.5f}; }
struct MovieOpt {
    bool gfx = true, small = true, smallSheet = true, shapes = false, shortRecord = false;
    uint32_t flags = 0x84;
    int sheetW = 1024, glyphs = kTestGlyphs;
    float badBox = 0.0f;  // not 0: the right edge of 'A' in the large font's sheet
    const char* file0 = "Birch Std MKLargeMenu.tga";
};
std::vector<uint8_t> MakeMovie(const MovieOpt& o) {
    std::vector<uint8_t> m(28, 0x11);  // what stands before the movie in its object
    const size_t g = m.size();
    m.insert(m.end(), {'G', 'F', (uint8_t)(o.gfx ? 'X' : 'Y'), 8});
    Put32(m, 0);
    m.push_back(16 << 3);
    for (int i = 0; i < 8; ++i) m.push_back(0);  // the stage's rectangle: 16 bits a side
    m.push_back(0), m.push_back(60), Put16(m, 1);
    const auto tag = [&](uint32_t code, const std::vector<uint8_t>& body, bool lengthOff = false) {
        if (body.size() < 63 && !lengthOff) {
            Put16(m, code << 6 | (uint32_t)body.size());
        } else {
            Put16(m, code << 6 | 63);
            Put32(m, (uint32_t)body.size() + (lengthOff ? 2 : 0));
        }
        m.insert(m.end(), body.begin(), body.end());
        if (lengthOff) m.push_back(0), m.push_back(0);
    };
    tag(1000, std::vector<uint8_t>(20, 0x33));  // the exporter's own record: a short tag, stepped over
    static const char* const kNames[2] = {"Birch Std MKLargeMenu", "Birch Std MKSmallMenu"};
    const auto font = [&](int which, const char* name, int id) {
        std::vector<uint8_t> b;
        Put16(b, (uint32_t)id);
        b.push_back((uint8_t)o.flags), b.push_back(1);
        const std::string stored = std::string(name) + " ";  // the exporter leaves a space and a NUL at the end
        b.push_back((uint8_t)(stored.size() + 1));
        b.insert(b.end(), stored.begin(), stored.end());
        b.push_back(0);
        Put16(b, (uint32_t)o.glyphs);
        Put16(b, o.shapes ? 8u : 0u), Put16(b, 4);
        for (int i = 0; i < o.glyphs; ++i) Put16(b, (uint32_t)TestCode(i));
        Put16(b, 18960), Put16(b, 5120), Put16(b, 3600);
        for (int i = 0; i < o.glyphs; ++i) Put16(b, (uint32_t)(TestAdvance(i) + which));
        for (int i = 0; i < o.glyphs; ++i) {  // a rectangle each: no bits, or eleven a side (seven bytes)
            const int bits = i % 3 ? 11 : 0;
            b.push_back((uint8_t)(bits << 3));
            for (int k = 1; k < (5 + 4 * bits + 7) / 8; ++k) b.push_back((uint8_t)(i + k));
        }
        Put16(b, 4);
        Put16(b, 'A'), Put16(b, 'V'), Put16(b, (uint32_t)(uint16_t)-1680);
        Put16(b, 'V'), Put16(b, 'A'), Put16(b, (uint32_t)(uint16_t)-1480);
        Put16(b, 0x151), Put16(b, 'A'), Put16(b, (uint32_t)(uint16_t)-100);  // a letter the reader does not take
        Put16(b, 'T'), Put16(b, 0x20), Put16(b, 40);
        tag(75, b, o.shortRecord && which == 1);
    };
    font(0, kNames[0], 7);
    font(2, "Agency FB_Bold", 3);  // a font the reader has no use for
    if (o.small) font(1, kNames[1], 9);
    const auto sheet = [&](int which, const char* file, int id) {
        std::vector<uint8_t> b;
        Put32(b, 393216u + (uint32_t)which), Put16(b, 13);
        b.push_back((uint8_t)strlen(file));
        b.insert(b.end(), file, file + strlen(file));
        Put16(b, (uint32_t)(which == 0 ? o.sheetW : 1024)), Put16(b, 512);
        b.push_back(kTestPad), Put16(b, kTestNominal), Put16(b, kTestInTexture);
        for (int t = 0; t < kTestInTexture; ++t) {
            const TestBox x = TestBoxOf(t);
            const bool bad = o.badBox != 0.0f && which == 0 && t == TestInTexture('A' - 0x20);
            PutF(b, (float)x.x / 1024.0f), PutF(b, (float)x.y / 512.0f), PutF(b, bad ? o.badBox : (float)(x.x + x.w) / 1024.0f), PutF(b, (float)(x.y + x.h) / 512.0f);
            PutF(b, ((float)x.x + x.ox) / 1024.0f), PutF(b, ((float)x.y + x.oy) / 512.0f);
        }
        Put16(b, 2);
        Put16(b, 77), Put16(b, 1), Put16(b, 0), Put16(b, 0);  // a font nobody asked for uses the sheet too
        int n = 0;
        for (int i = 0; i < o.glyphs; ++i) n += TestInTexture(i) >= 0;
        Put16(b, (uint32_t)id), Put16(b, (uint32_t)n);
        for (int i = 0; i < o.glyphs; ++i)
            if (TestInTexture(i) >= 0) Put16(b, (uint32_t)i), Put16(b, (uint32_t)TestInTexture(i));
        tag(1002, b);
    };
    sheet(0, o.file0, 7);
    if (o.small && o.smallSheet) sheet(1, "Birch Std MKSmallMenu.tga", 9);
    Put16(m, 0);
    Set32(m, g + 4, (uint32_t)(m.size() - g));
    return m;
}

// The standard package and, in front of it, everything the look needs: the movie first (near the file's start, where
// the damaged copies hit it), the two sheets of letters, the six frame pieces.
struct ArtPackOpt {
    MovieOpt movie;
    bool withMovie = true;
    const char* leaveOut = "";  // a texture's name
    int barHeight = 64, largeSheetW = 1024;
};
Pack StandardPack();
Pack ArtPack(const ArtPackOpt& o) {
    Pack p;
    p.withMovie = true;
    if (o.withMovie) {
        Thing t;
        t.name = "ui_ps_arenaSelect", t.texture = false, t.movie = true, t.raw = MakeMovie(o.movie);
        p.things.push_back(t);
    }
    for (const Thing& t : StandardPack().things) p.things.push_back(t);
    const auto art = [&](const char* name, int w, int h, int seed) {
        if (!strcmp(name, o.leaveOut)) return;
        Thing t;
        t.name = name, t.side = w, t.height = h, t.seed = seed, t.art = true;
        p.things.push_back(t);
    };
    art("Birch Std MKLargeMenu", o.largeSheetW, 512, 11), art("Birch Std MKSmallMenu", 1024, 512, 12);
    art("ui_ps_arenaSelect_I2F", 512, o.barHeight, 13), art("ui_ps_arenaSelect_I3B", 512, 64, 14), art("ui_ps_arenaSelect_I45", 128, 64, 15);
    art("ui_ps_arenaSelect_I51", 64, 64, 16), art("ui_ps_arenaSelect_I38", 256, 256, 17), art("ui_ps_arenaSelect_I16", 512, 512, 18);
    p.chunkBytes = 400000;
    return p;
}
const Thing* ThingOf(const Pack& p, const char* name) {
    for (const Thing& t : p.things)
        if (t.name == name) return &t;
    return nullptr;
}
// Pixels between two pages without access.
uint32_t* GuardedPixels(size_t count) {
    const size_t page = 4096, body = (count * 4 + page - 1) / page * page;
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, body + 2 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!base) {
        printf("FAIL no memory for guarded pixels\n");
        exit(2);
    }
    DWORD old;
    VirtualProtect(base, page, PAGE_NOACCESS, &old);
    VirtualProtect(base + page + body, page, PAGE_NOACCESS, &old);
    return reinterpret_cast<uint32_t*>(base + page + body - count * 4);  // the last pixel ends at the page without access
}

struct Mem {
    const std::vector<uint8_t>* v;
    long reads = 0, bad = 0;
};
bool MemRead(void* ctx, uint64_t at, void* buf, uint32_t n) {
    Mem* m = static_cast<Mem*>(ctx);
    ++m->reads;
    if (at > m->v->size() || n > m->v->size() - at) {
        ++m->bad;  // the reader asked for bytes the file does not have
        return false;
    }
    memcpy(buf, m->v->data() + at, n);
    return true;
}
bool LoadMem(const std::vector<uint8_t>& file, Set* out, long* badReads = nullptr, Art* art = nullptr) {
    Mem m{&file};
    const Source src{MemRead, &m, file.size()};
    const bool ok = Load(src, out, art);
    if (badReads) *badReads += m.bad;
    return ok;
}

// A picture of 0xAARRGGBB pixels as a 32-bit BMP, top row first.
bool SavePixels(const char* path, const uint32_t* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)w * (DWORD)h * 4;
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    const bool ok = fwrite(px, 4, (size_t)w * h, f) == (size_t)w * h;
    fclose(f);
    return ok;
}
bool SaveSheet(const char* path, const uint32_t* sheet) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)kSheetW * kSheetH * 4;
    ih.biSize = sizeof ih;
    ih.biWidth = kSheetW;
    ih.biHeight = -kSheetH;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(sheet, 4, (size_t)kSheetW * kSheetH, f);
    fclose(f);
    return true;
}

double NowMs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

Pack StandardPack() {
    Pack p;
    const auto tex = [&](const char* name, int seed) {
        Thing t;
        t.name = name, t.seed = seed;
        p.things.push_back(t);
        return &p.things.back();
    };
    tex("thumb_pit", 1)->marked = true;
    tex("big_pit", 9);                    // a texture, not a thumbnail
    tex("thumb_KOLISEUM", 2);             // capitals: read as small letters
    tex("thumb_small", 9)->side = 64;     // not 128 x 128: left out
    {
        Thing* t = tex("thumb_dxt1", 3);
        t->format = 5, t->marked = true;
    }
    {
        Thing t;  // not a texture; long enough for chunks of two blocks and objects across chunk borders
        t.name = "Filler", t.texture = false, t.filler = 150000;
        p.things.push_back(t);
    }
    tex("thumb_stgardenday", 4);
    tex("thumb_shangsthroneroom", 5);
    tex("thumb_packed", 9)->flags = 0x10;  // its data packed again: left out
    tex("thumb_pit", 9)->number = 1;       // "thumb_pit_0": a numbered name is no thumbnail
    tex("thumb_a_name_that_is_far_too_long_for_a_picture", 9);
    tex("thumb_format", 9)->format = 3;    // neither DXT1 nor DXT5: left out
    return p;
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* outDir = argc > 1 ? argv[1] : ".";
    CreateDirectoryA(outDir, nullptr);
    char text[400], path[MAX_PATH];

    // ---- 1. LZO1X ----
    {
        uint8_t out[64];
        const uint8_t a[] = {0x14, 'a', 'b', 'c', 0x48, 0x00, 0x11, 0x00, 0x00};  // three literals, then three bytes from three back
        Check(LzoDecompress(a, sizeof a, out, sizeof out) == 6 && !memcmp(out, "abcabc", 6), "LZO: literals and a match");
        const uint8_t b[] = {0x12, 'c', 0xE0, 0x00, 0x11, 0x00, 0x00};  // one literal, then eight bytes from one back (a run)
        Check(LzoDecompress(b, sizeof b, out, sizeof out) == 9 && !memcmp(out, "ccccccccc", 9), "LZO: a match that overlaps its own output");
        // "abcd", a match of 4 from 4 back with one literal after it ('X'), then a two-byte match from 5 back ("ab")
        const uint8_t c[] = {0x15, 'a', 'b', 'c', 'd', 0x6D, 0x00, 'X', 0x00, 0x01, 0x11, 0x00, 0x00};
        const int cn = LzoDecompress(c, sizeof c, out, sizeof out);
        snprintf(text, sizeof text, "%d bytes \"%.*s\"", cn, cn > 0 ? cn : 0, reinterpret_cast<const char*>(out));
        Check(cn == 11 && !memcmp(out, "abcdabcdXab", 11), "LZO: a match with a literal after it, and a two-byte match", text);
        const uint8_t end[] = {0x11, 0x00, 0x00};
        Check(LzoDecompress(end, 3, out, sizeof out) == 0, "LZO: an empty stream");
        Check(LzoDecompress(a, sizeof a, out, 5) == -1, "LZO: output that does not fit is refused");
        const uint8_t back[] = {0x12, 'c', 0xE0, 0x01, 0x11, 0x00, 0x00};  // a match from before the output's start
        Check(LzoDecompress(back, sizeof back, out, sizeof out) == -1, "LZO: a match from before the start is refused");
        const uint8_t more[] = {0x14, 'a', 'b', 'c', 0x11, 0x00, 0x00, 0x00};  // a byte after the end mark
        Check(LzoDecompress(more, sizeof more, out, sizeof out) == -1, "LZO: bytes after the end mark are refused");

        // Streams of literals, every length class, each read back whole; then every cut-off form of it.
        Rng rng{12345};
        bool whole = true, cut = true;
        int streams = 0;
        const Guarded gSrc(200000), gDst(200000), gPart(4096);
        for (const uint32_t n : {0u, 1u, 2u, 3u, 4u, 17u, 18u, 19u, 20u, 237u, 238u, 239u, 240u, 272u, 273u, 274u, 527u, 528u, 529u, 4096u, 65535u, 131072u}) {
            std::vector<uint8_t> data(n), packed;
            for (uint8_t& d : data) d = (uint8_t)rng.Next();
            Stored(data.data(), n, packed);
            uint8_t* src = gSrc.Tail(packed.size());
            memcpy(src, packed.data(), packed.size());
            uint8_t* dst = gDst.Tail(n);
            const int got = LzoDecompress(src, (uint32_t)packed.size(), dst, n);
            if (got != (int)n || memcmp(dst, data.data(), n)) {
                whole = false;
                printf("  a stream of %u literals gives %d bytes\n", n, got);
            }
            ++streams;
            if (n > 600) continue;
            for (size_t len = 0; len < packed.size(); ++len) {  // the stream cut after len bytes, ending at the guard
                uint8_t* part = gPart.Tail(len);
                memcpy(part, packed.data(), len);
                if (LzoDecompress(part, (uint32_t)len, dst, n) != -1) cut = false;
            }
            if (n) cut = cut && LzoDecompress(src, (uint32_t)packed.size(), gDst.Tail(n - 1), n - 1) == -1;
        }
        snprintf(text, sizeof text, "%d streams", streams);
        Check(whole, "LZO: a run of literals of every length class comes back whole", text);
        Check(cut, "LZO: every cut-off stream, and an output one byte short, is refused");

        // Streams of random instructions of every kind: unpacked they are what their writer kept. Then each with one
        // bit changed, and cut off: refused or not, never outside its buffers.
        {
            const Guarded gIn(1 << 20), gOut(4 << 20);
            int same = 0, made = 0, damagedTaken = 0;
            size_t longest = 0, packedBytes = 0, plainBytes = 0;
            for (int i = 0; i < 3000; ++i) {
                std::vector<uint8_t> s, want;
                RandomStream(rng, s, want, i < 2500 ? 1 + (int)rng.Below(40) : 200 + (int)rng.Below(3000));
                if (s.size() > (1 << 20) || want.size() > (4 << 20)) continue;
                ++made;
                packedBytes += s.size(), plainBytes += want.size();
                if (want.size() > longest) longest = want.size();
                uint8_t* in = gIn.Tail(s.size());
                memcpy(in, s.data(), s.size());
                uint8_t* out2 = gOut.Tail(want.size());
                const int got = LzoDecompress(in, (uint32_t)s.size(), out2, (uint32_t)want.size());
                if (got == (int)want.size() && !memcmp(out2, want.data(), want.size())) ++same;
                else if (same == made - 1) printf("  stream %d: %d bytes, wanted %u\n", i, got, (unsigned)want.size());
                if (i >= 2500) continue;
                for (int k = 0; k < 6; ++k) {
                    const size_t len = k < 4 ? s.size() : rng.Below((uint32_t)s.size());
                    uint8_t* d = gIn.Tail(len);
                    memcpy(d, s.data(), len);
                    if (k < 4 && len) d[rng.Below((uint32_t)len)] ^= (uint8_t)(1 << rng.Below(8));
                    const uint32_t cap = (uint32_t)want.size() + (k & 1 ? 0 : rng.Below(64));
                    const int g = LzoDecompress(d, (uint32_t)len, gOut.Tail(cap), cap);
                    damagedTaken += g >= 0;
                    if (g > (int)cap) same = -1000000;
                }
            }
            snprintf(text, sizeof text,
                     "%d of %d streams of random instructions unpack to what was packed (%u bytes from %u, the longest %u); %d of 15000 damaged ones were "
                     "still taken",
                     same, made, (unsigned)plainBytes, (unsigned)packedBytes, (unsigned)longest, damagedTaken);
            puts(text);
            Check(same == made && made >= 2900, "LZO: streams with every kind of match and run unpack exactly", text);
        }

        // Nonsense: no read past the input, no write past the output (either would hit the guard and stop the test).
        uint8_t* src = gSrc.Tail(256);
        uint8_t* dst = gDst.Tail(512);
        int accepted = 0;
        for (int i = 0; i < 100000; ++i) {
            const uint32_t n = 1 + rng.Below(256);
            uint8_t* s = src + 256 - n;
            for (uint32_t k = 0; k < n; ++k) s[k] = (uint8_t)(rng.Below(4) ? rng.Next() : rng.Below(3) ? 0 : 0x11);
            const uint32_t cap = rng.Below(513);
            const int got = LzoDecompress(s, n, dst + 512 - cap, cap);
            accepted += got >= 0;
            if (got > (int)cap) accepted = -1000000;
        }
        snprintf(text, sizeof text, "%d of 100000 taken as streams", accepted);
        Check(accepted >= 0, "LZO: 100000 streams of nonsense stay inside their buffers", text);
        puts(text);
    }

    uint32_t* sheet = GuardedSheet();
    Set set;
    set.sheet = sheet;

    // ---- 2. a package in MK9's layout ----
    std::vector<uint8_t> file;
    {
        Pack p = StandardPack();
        file = BuildFile(p, 777);
        long bad = 0;
        const bool ok = LoadMem(file, &set, &bad);
        snprintf(text, sizeof text, "%d pictures, %d textures, %d not usable, %d of %d chunks, %u bytes read, %u unpacked, file %u bytes: %s", set.count,
                 set.textures, set.skipped, set.chunksRead, set.chunks, (unsigned)set.fileBytes, (unsigned)set.unpackedBytes, (unsigned)file.size(), set.why);
        puts(text);
        Check(ok && set.count == 5 && bad == 0, "the test's package is read: five pictures", text);
        Check(set.version == 472 && set.licensee == 46 && set.exports == (int)p.things.size(), "its version and its export count are reported", text);
        Check(set.textures == 11 && set.skipped == 3, "eleven textures, three thumbnails not usable (not 128 x 128, packed again, another format)", text);
        Check(set.chunks >= 3 && set.chunksRead == set.chunks, "tables and objects cross chunk borders; every chunk was needed here", text);
        static const char* const kWant[] = {"pit", "koliseum", "dxt1", "stgardenday", "shangsthroneroom"};
        bool names = true;
        for (int i = 0; i < 5; ++i) names = names && !strcmp(set.name[i], kWant[i]) && Find(set, kWant[i]) == i;
        Check(names, "the pictures stand in the package's order under their names in small letters");
        Check(Find(set, "small") == -1 && Find(set, "packed") == -1 && Find(set, "") == -1 && Find(set, nullptr) == -1, "what was left out has no place");
        // Every pixel of every picture, at its place in the sheet.
        long wrong = 0;
        int slot = 0;
        for (const Thing& t : p.things) {
            if (slot >= set.count || strcmp(set.name[slot], kWant[slot])) break;
            std::string low = t.name;
            for (char& ch : low) ch = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
            if (low != std::string("thumb_") + kWant[slot] || t.number || t.side != 128 || t.flags || (t.format != 5 && t.format != 7)) continue;
            const int ox = (slot % 8) * kSide, oy = (slot / 8) * kSide;
            for (int y = 0; y < kSide; ++y)
                for (int x = 0; x < kSide; ++x) wrong += sheet[(size_t)(oy + y) * kSheetW + ox + x] != ExpectedPixel(t, x, y);
            ++slot;
        }
        snprintf(text, sizeof text, "%ld pixels differ, %d pictures compared", wrong, slot);
        Check(wrong == 0 && slot == 5, "every pixel of the five pictures is the one written (DXT5, DXT1, four-colour and three-colour blocks)", text);
        long notBlack = 0;
        for (int s = set.count; s < kSlots; ++s)
            for (int y = 0; y < kSide; ++y)
                for (int x = 0; x < kSide; ++x) notBlack += sheet[(size_t)((s / 8) * kSide + y) * kSheetW + (s % 8) * kSide + x] != 0xFF000000u;
        Check(notBlack == 0, "the sheet's unused places are black");
        snprintf(path, sizeof path, "%s\\arena_sheet_test.bmp", outDir);
        Check(SaveSheet(path, sheet), "the test's sheet is written as a picture", path);

        // The same package not packed.
        Pack plain = StandardPack();
        plain.plain = true;
        const std::vector<uint8_t> flat = BuildFile(plain, 777);
        Set s2;
        s2.sheet = GuardedSheet();
        const bool ok2 = LoadMem(flat, &s2, &bad);
        long differ = 0;
        for (size_t i = 0; i < (size_t)kSheetW * kSheetH; ++i) differ += s2.sheet[i] != sheet[i];
        snprintf(text, sizeof text, "%d pictures, %d chunks, %ld pixels differ: %s", s2.count, s2.chunks, differ, s2.why);
        Check(ok2 && s2.count == 5 && s2.chunks == 0 && differ == 0 && bad == 0, "the same package without chunks gives the same sheet", text);

        // More thumbnails than the sheet holds.
        Pack many;
        for (int i = 0; i < 34; ++i) {
            Thing t;
            snprintf(text, sizeof text, "thumb_arena%02d", i);
            t.name = text, t.seed = i;
            many.things.push_back(t);
        }
        many.chunkBytes = 1000000;
        const std::vector<uint8_t> big = BuildFile(many, 5);
        const bool ok3 = LoadMem(big, &s2, &bad);
        snprintf(text, sizeof text, "%d pictures, %d not usable: %s", s2.count, s2.skipped, s2.why);
        Check(ok3 && s2.count == kSlots && s2.skipped == 2 && !strcmp(s2.name[31], "arena31") && bad == 0, "34 thumbnails: 32 are read, two are counted as left out", text);

        // Refusals, each with its reason.
        const auto refused = [&](const char* what, std::vector<uint8_t> f, const char* reason) {
            Set s3;
            s3.sheet = s2.sheet;
            const bool ok4 = LoadMem(f, &s3, &bad);
            snprintf(text, sizeof text, "%s: %d pictures, \"%s\"", what, s3.count, s3.why);
            Check(!ok4 && s3.count == 0 && strstr(s3.why, reason), "a refusal says why", text);
        };
        std::vector<uint8_t> f = file;
        f[0] ^= 1;
        refused("another magic", f, "not an Unreal package");
        refused("an empty file", {}, "too short");
        f = file;
        Set32(f, 97 - 8, 1);  // the compression flags: zlib
        refused("packed another way", f, "not packed with LZO");
        f = file;
        Set32(f, 97 - 4, 5000);
        refused("5000 chunks", f, "chunk table");
        f = file;
        Set32(f, 33, 0x7FFFFFFF);  // the name count
        refused("two thousand million names", f, "sizes are not as expected");
        f = file;
        f.resize(f.size() / 2);
        refused("half the file", f, "chunk");
        Pack none;
        {
            Thing t;
            t.name = "big_only", t.seed = 1;
            none.things.push_back(t);
        }
        refused("no thumbnail", BuildFile(none, 3), "no readable thumb_ picture");
        Set nowhere;
        Check(!LoadMem(file, &nowhere) && nowhere.why[0], "no sheet to write into: refused");
        Check(bad == 0, "the reader never asked for bytes the file does not have");
    }

    // ---- 3. damaged copies ----
    {
        Rng rng{2026};
        Set s;
        s.sheet = sheet;  // between two guard pages
        long bad = 0;
        int read = 0, refusedN = 0, full = 0;
        bool sane = true;
        const int kCopies = 4000;
        for (int i = 0; i < kCopies; ++i) {
            std::vector<uint8_t> f = file;
            const int kind = i % 5;
            if (kind == 0) {  // a few bytes anywhere
                for (uint32_t k = 1 + rng.Below(8); k--;) f[rng.Below((uint32_t)f.size())] ^= (uint8_t)(1 + rng.Below(255));
            } else if (kind == 1) {  // the summary and the chunk table
                for (uint32_t k = 1 + rng.Below(4); k--;) f[rng.Below(260)] ^= (uint8_t)(1 + rng.Below(255));
            } else if (kind == 2) {  // cut off
                f.resize(rng.Below((uint32_t)f.size()));
            } else if (kind == 3) {  // the tables (the first chunk's blocks are literals: a changed byte is a changed table)
                for (uint32_t k = 1 + rng.Below(6); k--;) f[260 + rng.Below(1700)] ^= (uint8_t)(1 + rng.Below(255));
            } else {  // a number somewhere set to something large or negative
                const uint32_t at = rng.Below((uint32_t)f.size() - 4);
                static const uint32_t kValues[] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x00FFFFFFu, 0};
                Set32(f, kind == 4 && (i & 8) ? 20 + rng.Below(2000) : at, kValues[rng.Below(5)]);
            }
            const bool ok = LoadMem(f, &s, &bad);
            read += ok;
            refusedN += !ok;
            full += ok && s.count == 5;
            sane = sane && s.count >= 0 && s.count <= kSlots && (ok == (s.count > 0)) && s.why[sizeof s.why - 1] == 0;
            for (int k = 0; k < kSlots; ++k) sane = sane && memchr(s.name[k], 0, kNameLen) != nullptr;
        }
        snprintf(text, sizeof text, "%d damaged copies: %d refused, %d read (%d of them with all five pictures), %ld reads past the file's end", kCopies,
                 refusedN, read, full, bad);
        puts(text);
        Check(sane && bad == 0, "every damaged copy is read to an end, inside the file and inside the sheet", text);
        Check(refusedN > kCopies / 10 && read > kCopies / 10, "the copies were damaged enough to matter, and not all beyond use", text);
    }

    // ---- 3a. MK9's lettering and frame pieces ----
    uint32_t* artAtlas = GuardedPixels((size_t)kAtlasW * kAtlasH);
    uint32_t* artLetters = GuardedPixels((size_t)kFonts * kFontW * kFontH);
    const auto artInto = [&](Set* s, Art* a) {
        *s = Set{};
        *a = Art{};
        s->sheet = artAtlas, s->stride = kAtlasW;
        a->atlas = artAtlas;
        for (int i = 0; i < kFonts; ++i) a->font[i].px = artLetters + (size_t)i * kFontW * kFontH;
    };
    const auto artSane = [](const Art& a) {  // what the drawing code relies on
        if (a.why[sizeof a.why - 1] != 0) return false;
        if (!a.ok) return a.why[0] != 0;
        for (const Font& f : a.font) {
            if (!(f.em > 10.0f && f.em < 400.0f && f.cap >= 0.4f * f.em && f.cap <= f.em)) return false;
            for (const Glyph& g : f.glyph)
                if (g.x + g.w > kFontW || g.y + g.h > kFontH || g.w > 400 || g.h > 400 || !(fabsf(g.ox) < 4096.0f) || !(fabsf(g.oy) < 4096.0f) ||
                    !(fabsf(g.advance) < 2.0f))
                    return false;
            for (int ch = '0'; ch <= 'Z'; ++ch)
                if ((ch <= '9' || ch >= 'A') && (!f.glyph[ch - kFirstCode].w || !(f.glyph[ch - kFirstCode].advance > 0.0f))) return false;
        }
        return true;
    };
    Set artSet;
    Art artGood;
    {
        Pack p = ArtPack(ArtPackOpt{});
        const std::vector<uint8_t> artFile = BuildFile(p, 4242);
        const size_t lower = (size_t)kAtlasW * (kAtlasH / 2);
        for (size_t i = 0; i < (size_t)kAtlasW * kAtlasH; ++i) artAtlas[i] = 0x12345678u;  // what a load must clear above, and leave below
        artInto(&artSet, &artGood);
        long bad = 0;
        const double t0 = NowMs();
        const bool ok = LoadMem(artFile, &artSet, &bad, &artGood);
        const double ms = NowMs() - t0;
        const Font& large = artGood.font[kLargeMenu];
        const Font& small = artGood.font[kSmallMenu];
        snprintf(text, sizeof text,
                 "%d pictures; art %s%s%s; %d of %d chunks, %u bytes unpacked, file %u bytes, %.1f ms; letters %d and %d, kerning pairs %d and %d, EM %.3f, "
                 "capitals %.3f",
                 artSet.count, artGood.ok ? "read" : "NOT read: ", artGood.ok ? "" : artGood.why, "", artSet.chunksRead, artSet.chunks,
                 (unsigned)artSet.unpackedBytes, (unsigned)artFile.size(), ms, large.glyphs, small.glyphs, large.pairs, small.pairs, large.em, large.cap);
        puts(text);
        Check(ok && artGood.ok && !artGood.why[0] && artSet.count == 5 && bad == 0 && artSane(artGood), "a package with a movie, sheets of letters and frame pieces: all read", text);

        // The pictures: in the atlas' upper left quarter what a bare sheet gets from the package without any art.
        Set bare;
        bare.sheet = sheet;
        const bool okBare = LoadMem(file, &bare, &bad);
        long differ = 0;
        for (int y = 0; y < kSheetH; ++y)
            for (int x = 0; x < kSheetW; ++x) differ += artAtlas[(size_t)y * kAtlasW + x] != sheet[(size_t)y * kSheetW + x];
        bool names = okBare && bare.count == artSet.count;
        for (int i = 0; names && i < bare.count; ++i) names = !strcmp(bare.name[i], artSet.name[i]);
        snprintf(text, sizeof text, "%ld pixels differ", differ);
        Check(names && differ == 0, "the pictures stand in the atlas' corner exactly as in a sheet of their own", text);
        Set alone;
        alone.sheet = sheet;
        LoadMem(artFile, &alone, &bad);
        snprintf(text, sizeof text, "%d of %d chunks for the pictures alone, %d with the art", alone.chunksRead, alone.chunks, artSet.chunksRead);
        Check(alone.count == 5 && alone.chunksRead < artSet.chunksRead && !strcmp(alone.why, artSet.why), "without the art asked for, only the pictures' part of the file is read", text);

        // The letters: where each stands in its sheet, its pen, how far the pen moves; the first of two glyphs with one code.
        Check(fabsf(large.em - 87.5f) < 0.001f && fabsf(small.em - 87.5f) < 0.001f && large.cap == 63.5f && small.cap == 63.5f,
              "an EM is 87.5 sheet pixels for the nominal size 127; a capital is the 'H' box less its padding high");
        for (int which = 0; which < kFonts; ++which) {
            const Font& f = artGood.font[which];
            int wrongLetters = 0, kerned = 0;
            for (int k = 0; k < kCodes; ++k) {
                const Glyph& g = f.glyph[k];
                const float advance = (float)(int16_t)(TestAdvance(k) + which) / 20.0f / 1024.0f;
                if (k == 0) {
                    wrongLetters += g.w != 0 || g.advance != advance;
                    continue;
                }
                const TestBox b = TestBoxOf(TestInTexture(k));
                const bool same = g.x == b.x && g.y == b.y && g.w == b.w && g.h == b.h && g.ox == b.ox && g.oy == b.oy && g.advance == advance;
                if (!same && !wrongLetters) printf("  font %d letter '%c': box %d,%d %dx%d pen %.3f,%.3f advance %.5f\n", which, 0x20 + k, g.x, g.y, g.w, g.h, g.ox, g.oy, g.advance);
                wrongLetters += !same;
            }
            for (int a = 0; a < kCodes; ++a)
                for (int b = 0; b < kCodes; ++b) kerned += f.kern[a][b] != 0;
            snprintf(text, sizeof text, "font %d: %d letters differ, %d letters, %d pairs taken, %d in the table", which, wrongLetters, f.glyphs, f.pairs, kerned);
            Check(wrongLetters == 0 && f.glyphs == 63, "every letter's box, pen and advance are the movie's (a space has no picture; of two glyphs with one code the first counts)", text);
            Check(f.pairs == 3 && kerned == 3 && f.kern['A' - 0x20]['V' - 0x20] == -1680 && f.kern['V' - 0x20]['A' - 0x20] == -1480 && f.kern['T' - 0x20][0] == 40,
                  "the kerning pairs of the letters taken, and no other", text);
            const Thing* t = ThingOf(p, which ? "Birch Std MKSmallMenu" : "Birch Std MKLargeMenu");
            long wrong = 0;
            for (int y = 0; y < kFontH; ++y)
                for (int x = 0; x < kFontW; ++x) wrong += f.px[(size_t)y * kFontW + x] != ExpectedArt(*t, x, y);
            snprintf(text, sizeof text, "font %d: %ld pixels differ", which, wrong);
            Check(wrong == 0, "a sheet of letters: every pixel with its alpha, the colours multiplied by it", text);
        }

        // The frame pieces at their places, clear between them, the lower half left alone.
        struct PieceOf {
            PieceId id;
            const char* name;
            int by;  // kept at 1 / by of its size
        };
        static const PieceOf kPieceOf[] = {{kBar, "ui_ps_arenaSelect_I2F", 1},  {kSun, "ui_ps_arenaSelect_I3B", 1},  {kGlow, "ui_ps_arenaSelect_I45", 1},
                                           {kHalo, "ui_ps_arenaSelect_I51", 1}, {kRing, "ui_ps_arenaSelect_I38", 1}, {kRed, "ui_ps_arenaSelect_I16", 4}};
        std::vector<uint8_t> owned((size_t)(kAtlasW / 2) * (kAtlasH / 2), 0);
        long wrongPiece = 0, notWhite = 0, notClear = 0, touched = 0;
        bool apart = true;
        for (int i = 0; i < kPieces; ++i) {
            const Place a = PlaceOf((PieceId)i);
            apart = apart && a.w > 0 && a.h > 0 && a.x > kAtlasW / 2 && a.y > 0 && a.x + a.w < kAtlasW && a.y + a.h < kAtlasH / 2;
            for (int k = i + 1; k < kPieces; ++k) {  // a clear pixel at least between any two: a smoothed edge never takes a neighbour's colour
                const Place b = PlaceOf((PieceId)k);
                apart = apart && (a.x + a.w < b.x || b.x + b.w < a.x || a.y + a.h < b.y || b.y + b.h < a.y);
            }
            for (int y = 0; y < a.h; ++y)
                for (int x = 0; x < a.w; ++x) owned[(size_t)(a.y + y) * (kAtlasW / 2) + (a.x - kAtlasW / 2 + x)] = 1;
        }
        Check(apart && PlaceOf((PieceId)-1).w == 0 && PlaceOf(kPieces).w == 0, "the pieces' places lie apart in the atlas' upper right quarter");
        for (const PieceOf& k : kPieceOf) {
            const Thing* t = ThingOf(p, k.name);
            const Place a = PlaceOf(k.id);
            const int high = t->height ? t->height : t->side;
            if (a.w * k.by != t->side || a.h * k.by != high) wrongPiece += 1000000;
            for (int y = 0; y < a.h; ++y)
                for (int x = 0; x < a.w; ++x) wrongPiece += artAtlas[(size_t)(a.y + y) * kAtlasW + a.x + x] != ExpectedArt(*t, x * k.by, y * k.by);
        }
        const Place white = PlaceOf(kWhite);
        for (int y = 0; y < white.h; ++y)
            for (int x = 0; x < white.w; ++x) notWhite += artAtlas[(size_t)(white.y + y) * kAtlasW + white.x + x] != 0xFFFFFFFFu;
        for (int y = 0; y < kAtlasH / 2; ++y)
            for (int x = 0; x < kAtlasW / 2; ++x) notClear += !owned[(size_t)y * (kAtlasW / 2) + x] && artAtlas[(size_t)y * kAtlasW + kAtlasW / 2 + x] != 0;
        for (size_t i = lower; i < (size_t)kAtlasW * kAtlasH; ++i) touched += artAtlas[i] != 0x12345678u;
        snprintf(text, sizeof text, "%ld pixels of the pieces differ, %ld of the white block, %ld between them not clear, %ld of the lower half changed", wrongPiece,
                 notWhite, notClear, touched);
        Check(wrongPiece == 0, "every pixel of the six frame pieces at its place (the red glow as the mean of four by four)", text);
        Check(notWhite == 0 && notClear == 0, "a white block for plain fills; everything else of that quarter is clear", text);
        Check(touched == 0, "the atlas' lower half, the rendering thread's, is not written", text);
        snprintf(path, sizeof path, "%s\\arena_atlas_test.bmp", outDir);
        Check(SavePixels(path, artAtlas, kAtlasW, kAtlasH / 2), "the test's atlas is written as a picture", path);

        // Art in a form the reader does not expect: refused with its reason; the pictures are read all the same.
        const auto artRefused = [&](const char* what, const std::vector<uint8_t>& f, const char* reason) {
            Set s;
            Art a;
            artInto(&s, &a);
            const bool okPics = LoadMem(f, &s, &bad, &a);
            snprintf(text, sizeof text, "%s: %d pictures, art %s, \"%s\"", what, s.count, a.ok ? "read" : "not read", a.why);
            Check(okPics && s.count == 5 && !strcmp(s.why, artSet.why) && !a.ok && strstr(a.why, reason) && artSane(a),
                  "art that is not as expected is refused with its reason; the pictures are read all the same", text);
        };
        const auto refusedPack = [&](const char* what, const ArtPackOpt& o, const char* reason) {
            Pack q = ArtPack(o);
            artRefused(what, BuildFile(q, 4242), reason);
        };
        ArtPackOpt o;
        o.withMovie = false;
        refusedPack("no movie", o, "the package has no movie");
        o = ArtPackOpt{}, o.leaveOut = "ui_ps_arenaSelect_I38";
        refusedPack("no ring", o, "the package has no ui_ps_arenaSelect_I38");
        o = ArtPackOpt{}, o.barHeight = 32;
        refusedPack("a bar of another size", o, "ui_ps_arenaSelect_I2F is not the 512 x 64 picture expected");
        o = ArtPackOpt{}, o.leaveOut = "Birch Std MKSmallMenu";
        refusedPack("no sheet of the small letters", o, "the package has no sheet of letters 'Birch Std MKSmallMenu'");
        o = ArtPackOpt{}, o.largeSheetW = 512;
        refusedPack("a sheet of another size", o, "the sheet of letters 'Birch Std MKLargeMenu' is not as expected");
        o = ArtPackOpt{}, o.movie.file0 = "Another Sheet.tga";
        refusedPack("the movie names a sheet the package has not", o, "the package has no sheet of letters 'Another Sheet'");
        o = ArtPackOpt{}, o.movie.gfx = false;
        refusedPack("not a GFX movie", o, "not a GFX 8 movie");
        o = ArtPackOpt{}, o.movie.small = false;
        refusedPack("no small font", o, "the movie has no font 'Birch Std MKSmallMenu'");
        o = ArtPackOpt{}, o.movie.smallSheet = false;
        refusedPack("no texture for the small font", o, "the movie has no sheet for the font 'Birch Std MKSmallMenu'");
        o = ArtPackOpt{}, o.movie.shapes = true;
        refusedPack("a font with its outlines", o, "is not stored as expected");
        o = ArtPackOpt{}, o.movie.flags = 0x80;
        refusedPack("a font without its layout", o, "is not stored as expected (flags 80");
        o = ArtPackOpt{}, o.movie.shortRecord = true;
        refusedPack("a font record longer than its font", o, "'Birch Std MKSmallMenu' does not end where its record ends");
        o = ArtPackOpt{}, o.movie.sheetW = 512;
        refusedPack("a font texture of another size", o, "the sheet of 'Birch Std MKLargeMenu' is not as expected (512 x 512");
        o = ArtPackOpt{}, o.movie.badBox = 1.5f;
        refusedPack("a letter's box outside its sheet", o, "has no usable place in its sheet");
        o = ArtPackOpt{}, o.movie.badBox = nanf("");
        refusedPack("a letter's box that is not a number", o, "has no usable place in its sheet");
        o = ArtPackOpt{}, o.movie.glyphs = 58;
        refusedPack("a font without 'Z'", o, "has no letter 'Z'");
        o = ArtPackOpt{}, o.movie.glyphs = 40;
        refusedPack("a font without 'H'", o, "do not have the size expected");
        // In the file itself: the movie lies in a block of literals, so its bytes stand in the file as they are.
        const Thing* movie = ThingOf(p, "ui_ps_arenaSelect");
        const auto found = std::search(artFile.begin(), artFile.end(), movie->raw.begin(), movie->raw.end());
        Check(found != artFile.end(), "the test finds its movie in its file");
        const size_t movieAt = found == artFile.end() ? 0 : (size_t)(found - artFile.begin()), movieLen = movie->raw.size();
        std::vector<uint8_t> f = artFile;
        Set32(f, movieAt + 28 + 4, 0x7FFFFFFF);
        artRefused("a movie longer than its object", f, "the movie's length is not as expected");
        if (found == artFile.end()) {
            printf("%d checks, %d failed\n", gChecks, gFailed);
            return 1;
        }
        f = artFile;
        // After the 28 bytes before the movie: its head (8), the stage's rectangle (9), rate and frames (4), the exporter's
        // short tag (2 + 20), then the first font's tag, a long one: its length becomes two thousand million.
        Set32(f, movieAt + 28 + 8 + 9 + 4 + 22 + 2, 0x7FFFFFFF);
        artRefused("a tag longer than the movie", f, "the movie's tags are damaged");
        {
            Set s;
            Art a;
            artInto(&s, &a);
            a.font[kSmallMenu].px = nullptr;
            const bool okPics = LoadMem(artFile, &s, &bad, &a);
            Check(okPics && s.count == 5 && !a.ok && strstr(a.why, "nowhere to put the lettering"), "no memory for a sheet of letters: no art, the pictures all the same", a.why);
            Art none;  // no atlas at all, the pictures into a sheet of their own
            Set s2;
            s2.sheet = sheet;
            Check(LoadMem(artFile, &s2, &bad, &none) && s2.count == 5 && !none.ok && none.why[0], "no atlas: no art, the pictures all the same", none.why);
        }

        // Damaged copies: the movie's bytes, the tables, files cut off. Read to an end, inside the file and the pixels
        // (the atlas and the sheets of letters end at pages without access); what is called read is usable.
        Rng rng{7331};
        int artRead = 0, artNot = 0, picsRead = 0;
        bool sane = true;
        const int kArtCopies = 1200;
        const double d0 = NowMs();
        for (int i = 0; i < kArtCopies; ++i) {
            f = artFile;
            const int kind = i % 6;
            if (kind == 0) {  // a few bytes of the movie
                for (uint32_t k = 1 + rng.Below(3); k--;) f[movieAt + rng.Below((uint32_t)movieLen)] ^= (uint8_t)(1 + rng.Below(255));
            } else if (kind == 1) {  // a 16-bit number of the movie
                static const uint32_t kValues[] = {0xFFFF, 0, 0x7FFF, 0x8000, 1};
                const size_t at = movieAt + rng.Below((uint32_t)movieLen - 2);
                const uint32_t v = kValues[rng.Below(5)];
                f[at] = (uint8_t)v, f[at + 1] = (uint8_t)(v >> 8);
            } else if (kind == 2) {  // a 32-bit number of the movie
                static const uint32_t kValues[] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x00FFFFFFu, 0, 0x7FC00000u, 0xFF800000u};
                Set32(f, movieAt + rng.Below((uint32_t)movieLen - 4), kValues[rng.Below(7)]);
            } else if (kind == 3) {  // one bit of the movie
                f[movieAt + rng.Below((uint32_t)movieLen)] ^= (uint8_t)(1 << rng.Below(8));
            } else if (kind == 4) {  // the tables
                for (uint32_t k = 1 + rng.Below(4); k--;) f[260 + rng.Below((uint32_t)movieAt - 260)] ^= (uint8_t)(1 + rng.Below(255));
            } else {  // cut off
                f.resize(rng.Below((uint32_t)f.size()));
            }
            Set s;
            Art a;
            artInto(&s, &a);
            const bool okPics = LoadMem(f, &s, &bad, &a);
            artRead += a.ok, artNot += !a.ok, picsRead += okPics;
            sane = sane && artSane(a) && s.count >= 0 && s.count <= kSlots && (okPics == (s.count > 0));
        }
        snprintf(text, sizeof text, "%d damaged copies in %.1f s: the art read from %d, refused in %d; the pictures read from %d; %ld reads past the file's end",
                 kArtCopies, (NowMs() - d0) / 1000.0, artRead, artNot, picsRead, bad);
        puts(text);
        Check(sane && bad == 0, "every damaged copy is read to an end; art called read has every letter inside its sheet", text);
        Check(artNot > kArtCopies / 10 && artRead > kArtCopies / 10 && picsRead > artRead, "the copies were damaged enough to matter, and not all beyond use", text);

        // The good package once more: what the mod's own set is tested with below.
        for (size_t i = 0; i < (size_t)kAtlasW * kAtlasH; ++i) artAtlas[i] = 0;
        artInto(&artSet, &artGood);
        Check(LoadMem(artFile, &artSet, &bad, &artGood) && artGood.ok && bad == 0, "and the undamaged package is read again as before");
    }

    // ---- 4. the names ----
    {
        struct Named {
            const char* table;
            const char* picture;
        };
        static const Named kNames[] = {
            {"BGND_Courtyard", "courtyard"}, {"BGND_CourtyardN", "courtyardn"}, {"BGND_DeadPool", "deadpool"}, {"BGND_Desert", "desert"},
            {"BGND_FleshPits", "fleshpits"}, {"BGND_GorosLair", "goroslair"}, {"BGND_Graveyard", "graveyard"}, {"BGND_Hell", "hell"},
            {"BGND_KahnsColiseum", "koliseum"}, {"BGND_LivingForest", "livingforest"}, {"BGND_Pit", "pit"}, {"BGND_PitBottom", "pitbottom"},
            {"BGND_PitDay", "pitday"}, {"BGND_RooftopDawn", "rooftopdawn"}, {"BGND_RooftopDay", "rooftopday"}, {"BGND_RooftopDusk", "rooftopdusk"},
            {"BGND_SKThrone", "skthrone"}, {"BGND_SoulChamber", "soulchamber"}, {"BGND_STGardens", "stgardenday"}, {"BGND_STGardensN", "stgardensn"},
            {"BGND_Street", "street"}, {"BGND_STThrone", "shangsthroneroom"}, {"BGND_Subway", "subway"}, {"BGND_Temple", "temple"},
            {"BGND_Tower", "tower"}, {"BGND_Wastelands", "wastelands"}, {"BGND_Armory", "armory"}, {"BGND_BellTower", "belltower"},
            {"BGND_ChallengeTower", "challengetower"}, {"bgnd_pit", "pit"}, {"Pit", "pit"}, {"", ""}, {"BGND_", ""},
            {"BGND_AStageWithANameFarTooLongForAPicture", ""}};
        bool ok = true;
        for (const Named& n : kNames) {
            char out[kNameLen];
            PictureName(n.table, out, sizeof out);
            if (strcmp(out, n.picture)) {
                ok = false;
                printf("  \"%s\" gives \"%s\", wanted \"%s\"\n", n.table, out, n.picture);
            }
        }
        Check(ok, "a stage table entry names its picture (34 names, three of them by exception)");
        char tiny[8];
        memset(tiny, 0x7F, sizeof tiny);
        PictureName("BGND_LivingForest", tiny, 5);
        Check(strlen(tiny) == 4 && tiny[5] == 0x7F, "a short buffer is filled and not overrun");
        char none[4] = "abc";
        PictureName(nullptr, none, sizeof none);
        Check(none[0] == 0, "no name, no picture name");
    }

    // ---- 5. the mod's own set ----
    {
        Check(State() == kNotAsked && !Usable() && Count() == 0 && SlotOf("BGND_Pit") == -1 && Atlas() == nullptr && ArtOf() == nullptr,
              "before anything is asked: no pictures");
        Describe(text, sizeof text);
        Check(strstr(text, "not asked"), "and the status line says so", text);
        Request();  // beside this test's exe there is no Asset folder: the game's file is missing
        Request();
        for (int i = 0; i < 500 && State() == kLoading; ++i) Sleep(10);
        Describe(text, sizeof text);
        puts(text);
        Check(State() == kFailed && !Usable() && SlotOf("BGND_Pit") == -1, "the game's file is missing: no pictures, the stage screen keeps its names", text);
        Check(strstr(text, "ui_ps_arenaSelect.xxx") && strstr(text, "ui_ps_arenaSelect_n.xxx") && strstr(text, "names"), "the log line names both files tried", text);
        Shutdown();
        Shutdown();
        Adopt(&set);
        Check(State() == kReady && Usable() && Count() == 5 && SlotOf("BGND_Pit") == 0 && SlotOf("BGND_KahnsColiseum") == 1 && SlotOf("BGND_STGardens") == 3 &&
                  SlotOf("BGND_STThrone") == 4 && SlotOf("BGND_Hell") == -1,
              "a set taken over is found by stage table names");
        Check(Atlas() == nullptr && ArtOf() == nullptr, "a bare sheet of pictures is no atlas for the display, and gives no look");
        SetWanted(false);
        Describe(text, sizeof text);
        Check(!Usable() && strstr(text, "switched off"), "'stagepics 0': not shown, and said so", text);
        SetWanted(true);
        Check(Usable(), "'stagepics 1': shown again");
        Refuse("the device gave no texture");
        Refuse("a second reason");
        Describe(text, sizeof text);
        Check(!Usable() && strstr(text, "the device gave no texture"), "a refusal by the rendering thread: names from here on, with the first reason", text);
        Adopt(nullptr);
        Check(!Usable() && State() == kNotAsked && Atlas() == nullptr, "the set is given back");
        // A set in an atlas' corner, with the art: MK9's look, until it is switched off or the pictures are.
        Adopt(&artSet, &artGood);
        Check(Usable() && Atlas() == artAtlas && ArtOf() == &artGood, "a set with its art: the atlas for the display, and the look");
        SetLookWanted(false);
        Describe(text, sizeof text);
        Check(Usable() && ArtOf() == nullptr && Atlas() == artAtlas && strstr(text, "stagelook 0"), "'stagelook 0': plain tiles, and said so", text);
        SetLookWanted(true);
        Check(ArtOf() == &artGood, "'stagelook 1': the look again");
        SetWanted(false);
        Check(!Usable() && ArtOf() == nullptr, "'stagepics 0' takes the look with the pictures");
        SetWanted(true);
        Refuse("the device gave no texture");
        Check(ArtOf() == nullptr, "a refusal by the rendering thread ends the look too");
        Art noArt;
        Adopt(&artSet, &noArt);
        Check(Usable() && Atlas() == artAtlas && ArtOf() == nullptr, "pictures whose art could not be read: plain tiles");
        Adopt(nullptr);
        Check(Atlas() == nullptr && ArtOf() == nullptr, "and given back");
    }

    // ---- 6. the installed game's file ----
    if (argc > 2 && argv[2][0]) {
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, wide, MAX_PATH);
        Set real;
        real.sheet = sheet;
        const double t0 = NowMs();
        const bool ok = LoadFile(wide, &real);
        const double ms = NowMs() - t0;
        snprintf(text, sizeof text, "the game's file: %d pictures in %.1f ms, package version %d/%d, %d of %d chunks, %u bytes read, %u unpacked, %d textures, %d not usable%s%s",
                 real.count, ms, real.version, real.licensee, real.chunksRead, real.chunks, (unsigned)real.fileBytes, (unsigned)real.unpackedBytes,
                 real.textures, real.skipped, real.why[0] ? ": " : "", real.why);
        puts(text);
        Check(ok && real.count >= 28 && real.skipped == 0, "the installed game's pictures are read", text);
        Check(real.fileBytes < 2u << 20 && real.chunksRead <= 4, "from a small part of the file", text);
        static const char* const kStages[] = {"BGND_Courtyard", "BGND_CourtyardN", "BGND_DeadPool", "BGND_Desert", "BGND_FleshPits", "BGND_GorosLair",
                                              "BGND_Graveyard", "BGND_Hell", "BGND_KahnsColiseum", "BGND_LivingForest", "BGND_Pit", "BGND_PitBottom",
                                              "BGND_PitDay", "BGND_RooftopDawn", "BGND_RooftopDay", "BGND_RooftopDusk", "BGND_SKThrone", "BGND_SoulChamber",
                                              "BGND_STGardens", "BGND_STGardensN", "BGND_Street", "BGND_STThrone", "BGND_Subway", "BGND_Temple",
                                              "BGND_Tower", "BGND_Wastelands", "BGND_Armory", "BGND_BellTower"};
        int found = 0, flat = 0;
        for (const char* stage : kStages) {
            char name[kNameLen];
            PictureName(stage, name, sizeof name);
            const int slot = Find(real, name);
            if (slot < 0) {
                printf("  no picture for %s (%s)\n", stage, name);
                continue;
            }
            ++found;
            // A picture, not one colour: more than 200 different pixel values.
            std::vector<uint32_t> seen;
            for (int y = 0; y < kSide && seen.size() <= 200; y += 3)
                for (int x = 0; x < kSide && seen.size() <= 200; x += 3) {
                    const uint32_t c = sheet[(size_t)((slot / 8) * kSide + y) * kSheetW + (slot % 8) * kSide + x];
                    bool has = false;
                    for (const uint32_t k : seen) has = has || k == c;
                    if (!has) seen.push_back(c);
                }
            flat += seen.size() <= 200;
        }
        snprintf(text, sizeof text, "%d of 28 arenas have a picture, %d of them nearly one colour", found, flat);
        puts(text);
        Check(found == 28 && flat == 0, "each of the 28 arenas a player may pick has its picture", text);
        double sum = 0;
        for (int i = 0; i < 10; ++i) {
            const double a = NowMs();
            LoadFile(wide, &real);
            sum += NowMs() - a;
        }
        printf("ten more loads: %.1f ms each\n", sum / 10.0);
        Check(sum / 10.0 < 250.0, "a load takes under a quarter of a second");
        snprintf(path, sizeof path, "%s\\arena_sheet_game.bmp", outDir);
        Check(SaveSheet(path, sheet), "the sheet is written for a look (the game's art: it stays in build\\)", path);

        // Its lettering and frame pieces.
        Set realSet;
        Art realArt;
        for (size_t i = 0; i < (size_t)kAtlasW * kAtlasH; ++i) artAtlas[i] = 0;
        artInto(&realSet, &realArt);
        const double a0 = NowMs();
        const bool okArt = LoadFile(wide, &realSet, &realArt);
        const double artMs = NowMs() - a0;
        snprintf(text, sizeof text, "the game's file with its art: %d pictures, art %s%s in %.1f ms, %d of %d chunks, %u bytes read, %u unpacked", realSet.count,
                 realArt.ok ? "read" : "NOT read: ", realArt.ok ? "" : realArt.why, artMs, realSet.chunksRead, realSet.chunks, (unsigned)realSet.fileBytes,
                 (unsigned)realSet.unpackedBytes);
        puts(text);
        Check(okArt && realArt.ok && realSet.count == real.count && artSane(realArt), "the installed game's lettering and frame pieces are read", text);
        Check(artMs < 500.0, "in under half a second (on a thread of its own in the mod)", text);
        long differ = 0;
        for (int y = 0; y < kSheetH; ++y)
            for (int x = 0; x < kSheetW; ++x) differ += artAtlas[(size_t)y * kAtlasW + x] != sheet[(size_t)y * kSheetW + x];
        Check(differ == 0, "the pictures in the atlas' corner are the ones read alone");
        if (realArt.ok) {
            for (int which = 0; which < kFonts; ++which) {
                const Font& f = realArt.font[which];
                // Every letter and digit: a box with something drawn in it, no two boxes on the same pixels.
                int empty = 0, overlap = 0;
                for (int a = 0; a < kCodes; ++a) {
                    const Glyph& g = f.glyph[a];
                    if (!g.w) continue;
                    long ink = 0;
                    for (int y = 0; y < g.h; ++y)
                        for (int x = 0; x < g.w; ++x) ink += f.px[(size_t)(g.y + y) * kFontW + g.x + x] >> 24;
                    empty += ink == 0;
                    for (int b = a + 1; b < kCodes; ++b) {
                        const Glyph& h = f.glyph[b];
                        overlap += h.w && g.x < h.x + h.w && h.x < g.x + g.w && g.y < h.y + h.h && h.y < g.y + g.h;
                    }
                }
                const Glyph& A = f.glyph['A' - kFirstCode];
                const Glyph& H = f.glyph['H' - kFirstCode];
                snprintf(text, sizeof text,
                         "font %d: %d letters, %d kerning pairs, EM %.2f, capitals %.2f; 'A' box %d,%d %dx%d pen %.2f,%.2f advance %.0f/20480, AV %d, 'H' %dx%d; %d "
                         "empty boxes, %d overlaps",
                         which, f.glyphs, f.pairs, f.em, f.cap, A.x, A.y, A.w, A.h, A.ox, A.oy, A.advance * 20480.0f, f.kern['A' - kFirstCode]['V' - kFirstCode],
                         H.w, H.h, empty, overlap);
                puts(text);
                Check(f.glyphs >= 36 && fabsf(f.em - 87.5f) < 0.01f && f.cap > 55.0f && f.cap < 70.0f, "a font of the game: its letters, 87.5 pixels to the EM", text);
                // The same numbers as read from the file by another program before this reader existed (a PowerShell
                // script over the unpacked movie, 2026-11-13): both fonts are one typeface drawn twice.
                Check(fabsf(A.advance * 20480.0f - 6820.0f) < 0.5f && f.kern['A' - kFirstCode]['V' - kFirstCode] == -1680 && A.w == 41 && A.h == 76 && f.cap == 63.5f &&
                          f.pairs == 152,
                      "its 'A', its capitals' height and its kerning are the ones measured in the file by hand", text);
                Check(empty == 0 && overlap == 0, "every letter's box holds something drawn, and no two boxes share a pixel", text);
            }
            static const char* const kPieceName[kPieces] = {"the bar", "the sun line", "the glow", "the halo", "the ring", "the red glow", "the white block"};
            bool pieces = true;
            for (int i = 0; i < kPieces; ++i) {
                const Place a = PlaceOf((PieceId)i);
                long some = 0, clear = 0;
                for (int y = 0; y < a.h; ++y)
                    for (int x = 0; x < a.w; ++x) {
                        const uint32_t c = artAtlas[(size_t)(a.y + y) * kAtlasW + a.x + x];
                        some += (c >> 24) != 0, clear += (c >> 24) < 250;
                        // multiplied by alpha: no colour above its alpha
                        pieces = pieces && ((c >> 16) & 255) <= (c >> 24) && ((c >> 8) & 255) <= (c >> 24) && (c & 255) <= (c >> 24);
                    }
                printf("  %s: %ld of %d pixels drawn, %ld of them not opaque\n", kPieceName[i], some, a.w * a.h, clear - (a.w * a.h - some));
                pieces = pieces && some > a.w * a.h / 50 && (i == kWhite || clear > 0);
            }
            Check(pieces, "every frame piece holds something drawn with soft edges, its colours multiplied by its alpha");
            snprintf(path, sizeof path, "%s\\arena_atlas_game.bmp", outDir);
            Check(SavePixels(path, artAtlas, kAtlasW, kAtlasH / 2), "the atlas' upper half is written for a look (the game's art: it stays in build\\)", path);
        }
    } else {
        puts("the installed game's file was not given: read from the test's own package only");
    }

    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
