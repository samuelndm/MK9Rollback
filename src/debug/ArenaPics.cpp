#include "ArenaPics.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace ArenaPics;

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
int32_t I32(const uint8_t* p) { return (int32_t)U32(p); }

constexpr uint32_t kMagic = 0x9E2A83C1u;
// Limits far above the file looked at (chunks of at most 1 MB, a picture's object of 16664 bytes, 663 names): a file
// that asks for more is another file, and is left alone before anything large is allocated.
constexpr uint32_t kMaxChunk = 8u << 20;      // one chunk, packed or unpacked
constexpr uint32_t kMaxUnpacked = 24u << 20;  // everything this reader unpacks
constexpr uint32_t kMaxObject = 640u << 10;   // one object (a sheet of letters is 524568 bytes, the movie 177982)
constexpr uint32_t kMaxTable = 8u << 20;      // the name, import or export table
constexpr int kMaxChunks = 1024, kMaxNames = 200000, kMaxImports = 100000, kMaxExports = 100000;

// The letters' size. A font's record gives the sheet's "nominal size" (127); measured against the game's own menu
// (the widths of two menu lines in a frame of 2026-11-13, at the capitals' height drawn there) one EM is 87.5 of the
// sheet's pixels. Not from documentation.
constexpr float kEmOfNominal = 87.5f / 127.0f;

// The pieces' places in the atlas' upper right quarter, two pixels apart (what lies between them is transparent).
constexpr Place kPlaces[kPieces] = {{1026, 2, 512, 64},  {1026, 68, 512, 64},   {1026, 134, 128, 64}, {1156, 134, 64, 64},
                                    {1026, 200, 256, 256}, {1284, 200, 128, 128}, {1222, 134, 16, 16}};

// Bytes of the movie, every read checked: after a read past the end 'bad' stays set and every read gives 0.
struct Cur {
    const uint8_t* b;
    uint32_t n, o = 0;
    bool bad = false;
    bool Has(uint32_t k) {
        if (bad || o > n || n - o < k) bad = true;
        return !bad;
    }
    uint32_t U8() { return Has(1) ? b[o++] : 0; }
    uint32_t U16() {
        if (!Has(2)) return 0;
        o += 2;
        return (uint32_t)(b[o - 2] | b[o - 1] << 8);
    }
    uint32_t U32() {
        if (!Has(4)) return 0;
        o += 4;
        return ::U32(b + o - 4);
    }
    float F32() {
        const uint32_t v = U32();
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    void Skip(uint32_t k) {
        if (Has(k)) o += k;
    }
};

struct Chunk {
    uint32_t uAt = 0, uSize = 0, cAt = 0, cSize = 0;  // where it stands unpacked, and in the file
    bool tried = false, ok = false;
    std::vector<uint8_t> data;
};

class Package {
public:
    Package(const Source& src, Set* out, Art* art) : src_(src), out_(out), art_(art) {}

    bool Run() {
        if (!Open() || !ReadTables()) {
            if (art_) snprintf(art_->why, sizeof art_->why, "%s", out_->why);
            return false;
        }
        ReadPictures();
        if (out_->count == 0 && !out_->why[0]) Fail("the package holds no readable thumb_ picture");
        if (art_) {
            // The art's own failures are the art's: a chunk that does not unpack for it must not become the pictures' reason.
            char keep[sizeof out_->why];
            memcpy(keep, out_->why, sizeof keep);
            art_->ok = ReadArt();
            if (!art_->ok && !art_->why[0]) snprintf(art_->why, sizeof art_->why, "%s", out_->why[0] ? out_->why : "not readable");
            memcpy(out_->why, keep, sizeof keep);
        }
        return out_->count > 0;
    }

private:
    Source src_;
    Set* out_;
    Art* art_;
    std::vector<Chunk> chunks_;
    std::vector<uint8_t> names_, imports_, exports_, object_;
    std::vector<uint32_t> nameAt_;   // into names_: the text of name i (0xFFFFFFFF: not plain text)
    std::vector<uint32_t> nameLen_;
    int32_t nameCount_ = 0, importCount_ = 0, exportCount_ = 0;
    uint32_t nameOff_ = 0, importOff_ = 0, exportOff_ = 0;

    bool Fail(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(out_->why, sizeof out_->why, fmt, a);
        va_end(a);
        return false;
    }
    bool Read(uint64_t at, void* buf, uint32_t n) {
        if (at > src_.size || n > src_.size - at) return false;
        return n == 0 || src_.read(src_.ctx, at, buf, n);
    }

    // The summary, as far as this reader needs it, and the table of chunks.
    bool Open() {
        if (src_.size < 96) return Fail("the file is too short");
        const uint32_t headN = (uint32_t)std::min<uint64_t>(src_.size, 64 * 1024);
        std::vector<uint8_t> head(headN);
        if (!Read(0, head.data(), headN)) return Fail("the file's start cannot be read");
        const uint8_t* h = head.data();
        if (U32(h) != kMagic) return Fail("not an Unreal package");
        out_->version = h[4] | h[5] << 8;
        out_->licensee = h[6] | h[7] << 8;
        uint32_t o = 20;                 // after the header size, the tag "MK  " and one number
        const int32_t folder = I32(h + o);  // a counted text ("None")
        o += 4;
        if (folder < 0 || folder > 256 || o + (uint32_t)folder + 68 > headN) return Fail("the summary is not the one MK9 writes");
        o += (uint32_t)folder;
        o += 4;  // package flags
        nameCount_ = I32(h + o), nameOff_ = U32(h + o + 4);
        exportCount_ = I32(h + o + 8), exportOff_ = U32(h + o + 12);
        importCount_ = I32(h + o + 16), importOff_ = U32(h + o + 20);
        o += 28;          // those six and the depends offset
        o += 4 + 16 + 8;  // one number, the package's GUID, two numbers
        const uint32_t compression = U32(h + o);
        const int32_t n = I32(h + o + 4);
        o += 8;
        if (n < 0 || n > kMaxChunks || (uint64_t)o + (uint64_t)n * 16 > headN) return Fail("the chunk table is not as expected (%d chunks)", (int)n);
        if (n > 0 && !(compression & 2)) return Fail("the package is not packed with LZO (compression %u)", (unsigned)compression);
        chunks_.resize((size_t)n);
        for (int i = 0; i < n; ++i, o += 16) {
            Chunk& c = chunks_[(size_t)i];
            c.uAt = U32(h + o), c.uSize = U32(h + o + 4), c.cAt = U32(h + o + 8), c.cSize = U32(h + o + 12);
            const bool after = i == 0 || (c.uAt >= chunks_[(size_t)i - 1].uAt && c.uAt - chunks_[(size_t)i - 1].uAt >= chunks_[(size_t)i - 1].uSize);
            if (!after || c.uSize < 1 || c.uSize > kMaxChunk || c.cSize < 24 || c.cSize > kMaxChunk || (uint64_t)c.cAt + c.cSize > src_.size ||
                c.uAt > 0xFFFFFFFFu - c.uSize)
                return Fail("chunk %d of the chunk table is not as expected", i);
        }
        out_->chunks = n;
        return true;
    }

    // One chunk of the file: magic, block size, packed size, unpacked size, then (packed, unpacked) per block, then the
    // blocks, each LZO1X on its own.
    bool Inflate(Chunk& c) {
        if (c.tried) return c.ok;
        c.tried = true;
        if (out_->unpackedBytes > kMaxUnpacked - c.uSize) return false;
        std::vector<uint8_t> raw(c.cSize);
        if (!Read(c.cAt, raw.data(), c.cSize)) return false;
        const uint8_t* p = raw.data();
        if (U32(p) != kMagic) return false;
        const uint32_t block = U32(p + 4), unpacked = U32(p + 12);
        if (unpacked != c.uSize || block < 1024 || block > kMaxChunk) return false;
        const uint32_t blocks = (unpacked + block - 1) / block;
        if (blocks > 8192 || 16 + blocks * 8 > c.cSize) return false;
        c.data.resize(c.uSize);
        uint32_t at = 16 + blocks * 8, done = 0;
        for (uint32_t i = 0; i < blocks; ++i) {
            const uint32_t cs = U32(p + 16 + i * 8), us = U32(p + 16 + i * 8 + 4);
            if (cs > c.cSize - at || us > c.uSize - done || us > block) return false;
            if (LzoDecompress(p + at, cs, c.data.data() + done, us) != (int)us) return false;
            at += cs, done += us;
        }
        if (done != c.uSize || at != c.cSize) return false;
        out_->unpackedBytes += c.uSize;
        out_->fileBytes += c.cSize;
        ++out_->chunksRead;
        return c.ok = true;
    }

    // n bytes of the package as it is unpacked. A package without chunks is the file itself. Before the first chunk
    // lies the summary as the unpacked package has it, which the file does not hold in that form: never read.
    bool Get(uint32_t at, uint32_t n, uint8_t* to) {
        if (chunks_.empty()) return Read(at, to, n);
        while (n) {
            if (at < chunks_[0].uAt) return false;
            // the last chunk that starts at or before 'at'
            size_t lo = 0, hi = chunks_.size();
            while (hi - lo > 1) {
                const size_t mid = (lo + hi) / 2;
                if (chunks_[mid].uAt <= at) lo = mid;
                else hi = mid;
            }
            Chunk& c = chunks_[lo];
            if (at - c.uAt >= c.uSize) return false;  // a hole between two chunks
            if (!Inflate(c)) {
                if (!out_->why[0]) Fail("chunk %d of the file does not unpack", (int)lo);
                return false;
            }
            const uint32_t off = at - c.uAt, take = std::min(n, c.uSize - off);
            memcpy(to, c.data.data() + off, take);
            at += take, to += take, n -= take;
        }
        return true;
    }

    bool NameIs(int32_t index, const char* text) const {
        if (index < 0 || index >= nameCount_ || nameAt_[(size_t)index] == 0xFFFFFFFFu) return false;
        const size_t len = strlen(text);
        return nameLen_[(size_t)index] == len && !memcmp(&names_[nameAt_[(size_t)index]], text, len);
    }

    // The three tables. A name is a counted text and 8 bytes of flags, an import 28 bytes, an export 64 bytes.
    bool ReadTables() {
        if (nameCount_ < 1 || nameCount_ > kMaxNames || importCount_ < 1 || importCount_ > kMaxImports || exportCount_ < 1 || exportCount_ > kMaxExports)
            return Fail("the tables' sizes are not as expected (%d names, %d imports, %d exports)", (int)nameCount_, (int)importCount_, (int)exportCount_);
        if (importOff_ <= nameOff_ || importOff_ - nameOff_ > kMaxTable) return Fail("the name table has no usable place");
        names_.resize(importOff_ - nameOff_);
        if (!Get(nameOff_, (uint32_t)names_.size(), names_.data())) return out_->why[0] ? false : Fail("the name table cannot be read");
        nameAt_.assign((size_t)nameCount_, 0xFFFFFFFFu);
        nameLen_.assign((size_t)nameCount_, 0);
        const uint32_t end = (uint32_t)names_.size();
        uint32_t o = 0;
        for (int32_t i = 0; i < nameCount_; ++i) {
            if (end - o < 4) return Fail("the name table ends early (name %d)", (int)i);
            const int32_t len = I32(&names_[o]);
            o += 4;
            if (len < -65536 || len > 65536) return Fail("the name table is damaged (name %d)", (int)i);
            const uint32_t bytes = len >= 0 ? (uint32_t)len : (uint32_t)(-len) * 2;  // negative: two bytes per character
            if (bytes > end - o || end - o - bytes < 8) return Fail("the name table ends early (name %d)", (int)i);
            if (len > 0 && names_[o + (uint32_t)len - 1] == 0) nameAt_[(size_t)i] = o, nameLen_[(size_t)i] = (uint32_t)len - 1;
            o += bytes + 8;
        }
        imports_.resize((size_t)importCount_ * 28);
        if (!Get(importOff_, (uint32_t)imports_.size(), imports_.data())) return out_->why[0] ? false : Fail("the import table cannot be read");
        exports_.resize((size_t)exportCount_ * 64);
        if (!Get(exportOff_, (uint32_t)exports_.size(), exports_.data())) return out_->why[0] ? false : Fail("the export table cannot be read");
        out_->exports = exportCount_;
        return true;
    }

    void ReadPictures() {
        for (int32_t i = 0; i < exportCount_; ++i) {
            const uint8_t* e = &exports_[(size_t)i * 64];
            const int32_t cls = I32(e);  // negative: import -n-1
            if (cls >= 0 || -(int64_t)cls - 1 >= importCount_) continue;
            const uint8_t* imp = &imports_[(size_t)(-(int64_t)cls - 1) * 28];
            if (!NameIs(I32(imp + 20), "Texture2D") || I32(imp + 24) != 0) continue;
            ++out_->textures;
            const int32_t name = I32(e + 12), number = I32(e + 16);
            if (number != 0 || name < 0 || name >= nameCount_ || nameAt_[(size_t)name] == 0xFFFFFFFFu) continue;
            const char* text = reinterpret_cast<const char*>(&names_[nameAt_[(size_t)name]]);
            const uint32_t len = nameLen_[(size_t)name];
            if (len <= 6 || len - 6 >= (uint32_t)kNameLen || _strnicmp(text, "thumb_", 6)) continue;
            if (out_->count >= kSlots) {
                ++out_->skipped;
                continue;
            }
            const int slot = out_->count;
            uint32_t* px = out_->sheet + (size_t)(slot / 8) * kSide * (size_t)out_->stride + (size_t)(slot % 8) * kSide;
            if (!Texture(U32(e + 36), U32(e + 32), kSide, kSide, false, px, out_->stride)) {
                ++out_->skipped;
                continue;
            }
            char* to = out_->name[slot];
            for (uint32_t k = 0; k < len - 6; ++k) to[k] = text[6 + k] >= 'A' && text[6 + k] <= 'Z' ? (char)(text[6 + k] + 32) : text[6 + k];
            to[len - 6] = 0;
            ++out_->count;
        }
    }

    // A Texture2D: its tagged properties (name, type, size, array index, value) up to "None"; two empty data headers
    // (16 bytes each, the last number being the place right after it); the number of sizes; then per size a data header
    // (flags, bytes, bytes in the file, the data's place), the data, width and height. In the file looked at the data
    // lies inside the object and is not packed again (flags 0). The picture must be w x h; it is written to px in
    // rows 'stride' apart, with its alpha (and its colours multiplied by it) where 'alpha' says so, else opaque.
    bool Texture(uint32_t at, uint32_t size, int w, int h, bool alpha, uint32_t* px, int stride) {
        if (size < 64 || size > kMaxObject || at > 0xFFFFFFFFu - size) return false;
        object_.resize(size);
        if (!Get(at, size, object_.data())) return false;
        const uint8_t* b = object_.data();
        uint32_t o = 0;
        int format = -1;
        for (int guard = 0;; ++guard) {
            if (guard >= 64 || size - o < 8) return false;
            const int32_t name = I32(b + o), number = I32(b + o + 4);
            o += 8;
            if (number == 0 && NameIs(name, "None")) break;
            if (size - o < 16) return false;
            const int32_t type = I32(b + o), bytes = I32(b + o + 8);
            o += 16;
            if (bytes < 0) return false;
            if (NameIs(type, "StructProperty")) {  // the struct's name comes first
                if (size - o < 8) return false;
                o += 8;
            }
            if (NameIs(type, "BoolProperty")) {  // four bytes, not counted in its size
                if (size - o < 4) return false;
                o += 4;
                continue;
            }
            if ((uint32_t)bytes > size - o) return false;
            if (bytes == 1 && NameIs(name, "Format") && NameIs(type, "ByteProperty")) format = b[o];
            o += (uint32_t)bytes;
        }
        for (int i = 0; i < 2; ++i) {
            if (size - o < 16) return false;
            const uint32_t bytes = U32(b + o + 8);
            if (U32(b + o + 12) != at + o + 16 || bytes > size - o - 16) return false;
            o += 16 + bytes;
        }
        if (size - o < 20) return false;
        const int32_t sizes = I32(b + o);
        o += 4;
        if (sizes < 1 || sizes > 16) return false;
        const uint32_t flags = U32(b + o), count = U32(b + o + 4), bytes = U32(b + o + 8), place = U32(b + o + 12);
        o += 16;
        if (flags != 0 || count != bytes || place != at + o || bytes > size - o || size - o - bytes < 8) return false;
        const uint8_t* data = b + o;
        o += bytes;
        const bool dxt1 = format == 5;  // 5: DXT1, 7: DXT5
        if (I32(b + o) != w || I32(b + o + 4) != h || (format != 5 && format != 7)) return false;
        if (bytes != (uint32_t)(w / 4) * (uint32_t)(h / 4) * (dxt1 ? 8u : 16u)) return false;
        Decode(data, dxt1, alpha && !dxt1, w, h, px, stride);
        return true;
    }

    // The arena pictures: the colours only (their own alpha holds something else, 0..255 all over the picture, and is
    // left out). The lettering and the frame pieces: with DXT5's alpha, the colours multiplied by it.
    static void Decode(const uint8_t* d, bool dxt1, bool alpha, int w, int h, uint32_t* px, int stride) {
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4, d += dxt1 ? 8 : 16) {
                if (alpha) {
                    // Eight alphas: the two given and six between them, or four between them, 0 and 255.
                    int a[8];
                    a[0] = d[0], a[1] = d[1];
                    for (int i = 2; i < 8; ++i)
                        a[i] = a[0] > a[1] ? ((8 - i) * a[0] + (i - 1) * a[1]) / 7 : i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 : i == 6 ? 0 : 255;
                    const uint8_t* c = d + 8;
                    const int c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
                    const int r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
                    const int r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
                    const int r[4] = {r0, r1, (2 * r0 + r1) / 3, (r0 + 2 * r1) / 3}, g[4] = {g0, g1, (2 * g0 + g1) / 3, (g0 + 2 * g1) / 3},
                              bl[4] = {b0, b1, (2 * b0 + b1) / 3, (b0 + 2 * b1) / 3};
                    const uint32_t idx = U32(c + 4);
                    const uint64_t bits = (uint64_t)d[2] | (uint64_t)d[3] << 8 | (uint64_t)d[4] << 16 | (uint64_t)d[5] << 24 | (uint64_t)d[6] << 32 |
                                          (uint64_t)d[7] << 40;
                    for (int i = 0; i < 16; ++i) {
                        const int k = (int)((idx >> (2 * i)) & 3), al = a[(bits >> (3 * i)) & 7];
                        px[(size_t)(by + (i >> 2)) * (size_t)stride + bx + (i & 3)] = (uint32_t)al << 24 | (uint32_t)((r[k] * al + 127) / 255) << 16 |
                                                                                      (uint32_t)((g[k] * al + 127) / 255) << 8 | (uint32_t)((bl[k] * al + 127) / 255);
                    }
                    continue;
                }
                const uint8_t* c = dxt1 ? d : d + 8;
                const int c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
                const int r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
                const int r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
                uint32_t col[4];
                col[0] = 0xFF000000u | (uint32_t)(r0 << 16 | g0 << 8 | b0);
                col[1] = 0xFF000000u | (uint32_t)(r1 << 16 | g1 << 8 | b1);
                if (!dxt1 || c0 > c1) {
                    col[2] = 0xFF000000u | (uint32_t)((2 * r0 + r1) / 3 << 16 | (2 * g0 + g1) / 3 << 8 | (2 * b0 + b1) / 3);
                    col[3] = 0xFF000000u | (uint32_t)((r0 + 2 * r1) / 3 << 16 | (g0 + 2 * g1) / 3 << 8 | (b0 + 2 * b1) / 3);
                } else {
                    col[2] = 0xFF000000u | (uint32_t)((r0 + r1) / 2 << 16 | (g0 + g1) / 2 << 8 | (b0 + b1) / 2);
                    col[3] = 0xFF000000u;
                }
                const uint32_t idx = U32(c + 4);
                for (int i = 0; i < 16; ++i) px[(size_t)(by + (i >> 2)) * (size_t)stride + bx + (i & 3)] = col[(idx >> (2 * i)) & 3];
            }
    }

    // ---- the lettering and the frame pieces ----
    bool ArtFail(const char* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(art_->why, sizeof art_->why, fmt, a);
        va_end(a);
        return false;
    }

    // The first export of a class (an import's name) and, where a name is given, of that name without a number.
    bool FindExport(const char* cls, const char* name, uint32_t* at, uint32_t* size) const {
        for (int32_t i = 0; i < exportCount_; ++i) {
            const uint8_t* e = &exports_[(size_t)i * 64];
            const int32_t c = I32(e);
            if (c >= 0 || -(int64_t)c - 1 >= importCount_) continue;
            const uint8_t* imp = &imports_[(size_t)(-(int64_t)c - 1) * 28];
            if (!NameIs(I32(imp + 20), cls) || I32(imp + 24) != 0) continue;
            if (name && (I32(e + 16) != 0 || !NameIs(I32(e + 12), name))) continue;
            *at = U32(e + 36), *size = U32(e + 32);
            return true;
        }
        return false;
    }

    bool ReadArt() {
        Art& a = *art_;
        if (!a.atlas || !a.font[0].px || !a.font[1].px) return ArtFail("nowhere to put the lettering");
        struct Piece {
            PieceId id;
            const char* name;
            int w, h;
        };
        static const Piece kFromFile[] = {{kBar, "ui_ps_arenaSelect_I2F", 512, 64},   {kSun, "ui_ps_arenaSelect_I3B", 512, 64},
                                          {kGlow, "ui_ps_arenaSelect_I45", 128, 64},  {kHalo, "ui_ps_arenaSelect_I51", 64, 64},
                                          {kRing, "ui_ps_arenaSelect_I38", 256, 256}, {kRed, "ui_ps_arenaSelect_I16", 512, 512}};
        std::vector<uint32_t> tmp;
        uint32_t at = 0, size = 0;
        for (const Piece& s : kFromFile) {
            if (!FindExport("Texture2D", s.name, &at, &size)) return ArtFail("the package has no %s", s.name);
            tmp.assign((size_t)s.w * s.h, 0);
            if (!Texture(at, size, s.w, s.h, true, tmp.data(), s.w)) return ArtFail("%s is not the %d x %d picture expected", s.name, s.w, s.h);
            const Place p = kPlaces[s.id];
            const int f = s.w / p.w;  // 1; 4 for the red glow, which is soft enough to be kept at a quarter
            for (int y = 0; y < p.h; ++y)
                for (int x = 0; x < p.w; ++x) {
                    uint32_t sum[4] = {};
                    for (int j = 0; j < f; ++j)
                        for (int i = 0; i < f; ++i) {
                            const uint32_t c = tmp[(size_t)(y * f + j) * s.w + (size_t)(x * f + i)];
                            sum[0] += c >> 24, sum[1] += (c >> 16) & 255, sum[2] += (c >> 8) & 255, sum[3] += c & 255;
                        }
                    const uint32_t n = (uint32_t)(f * f), half = n / 2;
                    a.atlas[(size_t)(p.y + y) * kAtlasW + p.x + x] =
                        (sum[0] + half) / n << 24 | (sum[1] + half) / n << 16 | (sum[2] + half) / n << 8 | (sum[3] + half) / n;
                }
        }
        const Place white = kPlaces[kWhite];
        for (int y = 0; y < white.h; ++y)
            for (int x = 0; x < white.w; ++x) a.atlas[(size_t)(white.y + y) * kAtlasW + white.x + x] = 0xFFFFFFFFu;

        if (!FindExport("MwyUIMovieData", nullptr, &at, &size)) return ArtFail("the package has no movie");
        if (size < 64 || size > kMaxObject || at > 0xFFFFFFFFu - size) return ArtFail("the movie's size is not as expected");
        std::vector<uint8_t> movie(size);
        if (!Get(at, size, movie.data())) return ArtFail("the movie cannot be read");
        char sheet[kFonts][64] = {};
        if (!ReadFonts(movie, sheet)) return false;
        for (int i = 0; i < kFonts; ++i) {
            if (!FindExport("Texture2D", sheet[i], &at, &size)) return ArtFail("the package has no sheet of letters '%s'", sheet[i]);
            if (!Texture(at, size, kFontW, kFontH, true, a.font[i].px, kFontW)) return ArtFail("the sheet of letters '%s' is not as expected", sheet[i]);
        }
        return true;
    }

    // The movie: "GFX", version 8, its length; the stage's rectangle, frame rate and count; then tags (a 16-bit head:
    // code and length, 63 meaning a 32-bit length follows). Two kinds are read. Tag 75, a font as the exporter stores
    // it with its outlines left out: id, flags, language, counted name, number of glyphs, an empty offset and the code
    // table's offset (4), a 16-bit code per glyph, ascent, descent, leading, a 16-bit advance per glyph (1/20 of a unit,
    // 1024 units per EM), a rectangle per glyph, the kerning pairs (code, code, correction). Tag 1002, a font's
    // texture: id, format, counted file name, width, height, padding, nominal size, per glyph in the texture six
    // floats (left, top, right, bottom, and the pen's place; all in parts of the texture), then per font using it
    // pairs (glyph in the font, glyph in the texture). Both layouts were read from the game's file (every record ends
    // exactly where its tag ends, which is checked here too) and are refused in any other form.
    bool ReadFonts(const std::vector<uint8_t>& m, char sheet[kFonts][64]) {
        static const char* const kName[kFonts] = {"Birch Std MKLargeMenu", "Birch Std MKSmallMenu"};
        uint32_t g = 0;
        for (;; ++g) {
            if (g >= 256 || g + 32 > m.size()) return ArtFail("the movie is not a GFX 8 movie");
            if (m[g] == 'G' && m[g + 1] == 'F' && m[g + 2] == 'X' && m[g + 3] == 8) break;
        }
        const uint32_t len = U32(&m[g + 4]);
        if (len < 32 || len > m.size() - g) return ArtFail("the movie's length is not as expected");
        Cur c{m.data() + g, len};
        c.o = 8;
        c.Skip((5 + 4 * (uint32_t)(c.b[8] >> 3) + 7) / 8);
        c.Skip(4);
        struct Tag {
            uint32_t code, at, len;
        };
        std::vector<Tag> tags;
        for (int guard = 0; c.o < len; ++guard) {
            const uint32_t head = c.U16();
            uint32_t n = head & 63;
            if (n == 63) n = c.U32();
            if (c.bad || n > len - c.o || guard > 100000) return ArtFail("the movie's tags are damaged");
            if (((head >> 6) == 75 || (head >> 6) == 1002) && tags.size() < 256) tags.push_back(Tag{head >> 6, c.o, n});
            c.o += n;
            if ((head >> 6) == 0) break;
        }
        int fontId[kFonts], glyphOf[kFonts][kCodes];
        bool sheetSeen[kFonts] = {};
        for (int i = 0; i < kFonts; ++i) {
            fontId[i] = -1;
            for (int& x : glyphOf[i]) x = -1;
            art_->font[i].glyphs = art_->font[i].pairs = 0;
            art_->font[i].em = art_->font[i].cap = 0;
            for (Glyph& gl : art_->font[i].glyph) gl = Glyph{};
            memset(art_->font[i].kern, 0, sizeof art_->font[i].kern);
        }
        for (const Tag& t : tags) {
            if (t.code != 75) continue;
            Cur f{c.b + t.at, t.len};
            const uint32_t id = f.U16(), flags = f.U8();
            f.Skip(1);
            const uint32_t nameLen = f.U8(), nameAt = f.o;
            f.Skip(nameLen);
            if (f.bad) return ArtFail("a font of the movie is damaged");
            uint32_t trimmed = nameLen;
            while (trimmed && (f.b[nameAt + trimmed - 1] == 0 || f.b[nameAt + trimmed - 1] == ' ')) --trimmed;
            int which = -1;
            for (int i = 0; i < kFonts; ++i)
                if (fontId[i] < 0 && strlen(kName[i]) == trimmed && !memcmp(f.b + nameAt, kName[i], trimmed)) which = i;
            if (which < 0) continue;
            Font& font = art_->font[which];
            const uint32_t count = f.U16(), none = f.U16(), codesAt = f.U16();
            if (f.bad || (flags & 0x8C) != 0x84 || none != 0 || codesAt != 4 || count < 1 || count > 4096)
                return ArtFail("the font '%s' is not stored as expected (flags %02X, %u glyphs)", kName[which], (unsigned)flags, (unsigned)count);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t code = f.U16();
                if (code >= (uint32_t)kFirstCode && code < (uint32_t)(kFirstCode + kCodes) && glyphOf[which][code - kFirstCode] < 0)
                    glyphOf[which][code - kFirstCode] = (int)i;
            }
            f.Skip(6);  // ascent, descent, leading
            const uint32_t advancesAt = f.o;
            f.Skip(2 * count);
            for (uint32_t i = 0; i < count && !f.bad; ++i) f.Skip((5 + 4 * (uint32_t)(f.Has(1) ? f.b[f.o] >> 3 : 0) + 7) / 8);
            const uint32_t pairs = f.U16();
            if (f.bad || pairs > (t.len - f.o) / 6 || f.o + pairs * 6 != t.len) return ArtFail("the font '%s' does not end where its record ends", kName[which]);
            for (int k = 0; k < kCodes; ++k)
                if (glyphOf[which][k] >= 0) {
                    const uint8_t* p = f.b + advancesAt + 2 * (uint32_t)glyphOf[which][k];
                    font.glyph[k].advance = (float)(int16_t)(p[0] | p[1] << 8) / 20.0f / 1024.0f;
                }
            for (uint32_t i = 0; i < pairs; ++i) {
                const uint32_t left = f.U16(), right = f.U16();
                const int16_t by = (int16_t)f.U16();
                if (left >= (uint32_t)kFirstCode && left < (uint32_t)(kFirstCode + kCodes) && right >= (uint32_t)kFirstCode && right < (uint32_t)(kFirstCode + kCodes)) {
                    font.kern[left - kFirstCode][right - kFirstCode] = by;
                    ++font.pairs;
                }
            }
            fontId[which] = (int)id;
        }
        for (int i = 0; i < kFonts; ++i)
            if (fontId[i] < 0) return ArtFail("the movie has no font '%s'", kName[i]);
        for (const Tag& t : tags) {
            if (t.code != 1002) continue;
            Cur f{c.b + t.at, t.len};
            f.Skip(6);  // the texture's id and format
            const uint32_t nameLen = f.U8(), nameAt = f.o;
            f.Skip(nameLen);
            const uint32_t w = f.U16(), h = f.U16(), pad = f.U8(), nominal = f.U16(), inTexture = f.U16(), boxesAt = f.o;
            if (f.bad || inTexture > (t.len - f.o) / 24) return ArtFail("a font texture of the movie is damaged");
            f.Skip(24 * inTexture);
            const uint32_t users = f.U16();
            for (uint32_t u = 0; u < users && !f.bad; ++u) {
                const uint32_t id = f.U16(), count = f.U16(), pairsAt = f.o;
                if (f.bad || count > (t.len - f.o) / 4) return ArtFail("a font texture of the movie is damaged");
                f.Skip(4 * count);
                int which = -1;
                for (int i = 0; i < kFonts; ++i)
                    if (fontId[i] == (int)id && !sheetSeen[i]) which = i;
                if (which < 0) continue;
                Font& font = art_->font[which];
                uint32_t dot = nameLen;
                while (dot && f.b[nameAt + dot - 1] != '.') --dot;
                const uint32_t stem = dot ? dot - 1 : nameLen;
                if (w != (uint32_t)kFontW || h != (uint32_t)kFontH || pad > 32 || nominal < 16 || nominal > 512 || stem < 1 || stem > 63)
                    return ArtFail("the sheet of '%s' is not as expected (%u x %u, size %u)", kName[which], (unsigned)w, (unsigned)h, (unsigned)nominal);
                memcpy(sheet[which], f.b + nameAt, stem);
                sheet[which][stem] = 0;
                font.em = (float)nominal * kEmOfNominal;
                for (uint32_t k = 0; k < count; ++k) {
                    const uint8_t* p = f.b + pairsAt + 4 * k;
                    const uint32_t inFont = (uint32_t)(p[0] | p[1] << 8), inTex = (uint32_t)(p[2] | p[3] << 8);
                    int code = -1;
                    for (int q = 0; q < kCodes; ++q)
                        if (glyphOf[which][q] == (int)inFont) code = q;
                    if (code < 0) continue;
                    if (inTex >= inTexture) return ArtFail("a letter of '%s' points outside its sheet's table", kName[which]);
                    Cur b{f.b + boxesAt + 24 * inTex, 24};
                    float v[6];
                    bool sane = true;
                    for (float& x : v) {
                        x = b.F32();
                        sane = sane && x >= -1.0f && x <= 2.0f;  // false for anything that is not a number, too
                    }
                    const long x0 = sane ? lroundf(v[0] * (float)kFontW) : -1, y0 = sane ? lroundf(v[1] * (float)kFontH) : -1,
                               x1 = sane ? lroundf(v[2] * (float)kFontW) : -1, y1 = sane ? lroundf(v[3] * (float)kFontH) : -1;
                    if (!sane || x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 || x1 > kFontW || y1 > kFontH || x1 - x0 > 400 || y1 - y0 > 400)
                        return ArtFail("a letter of '%s' has no usable place in its sheet", kName[which]);
                    Glyph& gl = font.glyph[code];
                    gl.x = (uint16_t)x0, gl.y = (uint16_t)y0, gl.w = (uint16_t)(x1 - x0), gl.h = (uint16_t)(y1 - y0);
                    gl.ox = v[4] * (float)kFontW - (float)x0, gl.oy = v[5] * (float)kFontH - (float)y0;
                    ++font.glyphs;
                }
                // A capital's height: its box begins 'pad' pixels above the letter.
                const Glyph& capital = font.glyph['H' - kFirstCode];
                font.cap = capital.w ? capital.oy - (float)pad : 0.0f;
                if (!(font.cap >= 0.4f * font.em && font.cap <= font.em)) return ArtFail("the letters of '%s' do not have the size expected", kName[which]);
                sheetSeen[which] = true;
            }
            if (f.bad || f.o != t.len) return ArtFail("a font texture of the movie does not end where its record ends");
        }
        for (int i = 0; i < kFonts; ++i) {
            if (!sheetSeen[i]) return ArtFail("the movie has no sheet for the font '%s'", kName[i]);
            const Font& font = art_->font[i];
            for (int ch = '0'; ch <= 'Z'; ++ch)
                if ((ch <= '9' || ch >= 'A') && (!font.glyph[ch - kFirstCode].w || !(font.glyph[ch - kFirstCode].advance > 0.0f)))
                    return ArtFail("the font '%s' has no letter '%c'", kName[i], ch);
        }
        return true;
    }
};

struct FileCtx {
    HANDLE h;
};
bool FileRead(void* ctx, uint64_t at, void* buf, uint32_t n) {
    const HANDLE h = static_cast<FileCtx*>(ctx)->h;
    LARGE_INTEGER to;
    to.QuadPart = (LONGLONG)at;
    DWORD got = 0;
    return SetFilePointerEx(h, to, nullptr, FILE_BEGIN) && ReadFile(h, buf, n, &got, nullptr) && got == n;
}

// ---- the mod's one set ----
Set gSet;  // written by the reading thread, published through gUse when it is whole, never changed afterwards
Art gArt;  // the same, through gArtUse
// The pixels: the atlas (8 MB) and the two sheets of letters (2 MB each), taken from Windows by the reading thread
// and given back when the DLL unloads. Without the atlas there are no pictures; without the sheets, no lettering.
uint32_t* gAtlas = nullptr;
uint32_t* gLetters = nullptr;
std::atomic<const Set*> gUse{nullptr};
std::atomic<const Art*> gArtUse{nullptr};
volatile LONG gState = kNotAsked, gWanted = 1, gLookWanted = 1, gRefused = 0;
HANDLE gThread = nullptr;
char gLine[640] = "";
char gRefusal[100] = "";

void Keep(Set* out, Art* art) {  // a fresh Set and Art that keep the caller's pixels
    uint32_t* sheet = out->sheet;
    const int stride = out->stride;
    *out = Set{};
    out->sheet = sheet, out->stride = stride;
    if (!art) return;
    uint32_t* atlas = art->atlas;
    uint32_t* px[kFonts];
    for (int i = 0; i < kFonts; ++i) px[i] = art->font[i].px;
    *art = Art{};
    art->atlas = atlas;
    for (int i = 0; i < kFonts; ++i) art->font[i].px = px[i];
}

DWORD WINAPI ReadThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    gAtlas = static_cast<uint32_t*>(VirtualAlloc(nullptr, (size_t)kAtlasW * kAtlasH * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!gAtlas) {
        snprintf(gLine, sizeof gLine, "no arena pictures, the stage screen shows names (Windows gave no memory for them)");
        InterlockedExchange(&gState, kFailed);
        return 0;
    }
    gLetters = static_cast<uint32_t*>(VirtualAlloc(nullptr, (size_t)kFonts * kFontW * kFontH * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);  // the game's exe; its packages lie in Asset beside it
    while (n > 0 && n < MAX_PATH && dir[n - 1] != L'\\' && dir[n - 1] != L'/') --n;
    static const wchar_t* const kFiles[] = {L"Asset\\ui_ps_arenaSelect.xxx", L"Asset\\ui_ps_arenaSelect_n.xxx"};
    bool ok = false;
    char tried[300] = "";
    const wchar_t* from = L"";
    for (const wchar_t* file : kFiles) {
        if (n == 0 || n >= MAX_PATH || n + wcslen(file) + 1 > MAX_PATH) break;
        wchar_t path[MAX_PATH];
        memcpy(path, dir, n * sizeof(wchar_t));
        wcscpy(path + n, file);
        gSet = Set{};
        gSet.sheet = gAtlas, gSet.stride = kAtlasW;
        gArt = Art{};
        gArt.atlas = gAtlas;
        for (int i = 0; i < kFonts; ++i) gArt.font[i].px = gLetters ? gLetters + (size_t)i * kFontW * kFontH : nullptr;
        from = file;
        if ((ok = LoadFile(path, &gSet, &gArt))) break;
        const size_t at = strlen(tried);
        snprintf(tried + at, sizeof tried - at, "%s%ls: %s", at ? "; " : "", file, gSet.why);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)(fq.QuadPart ? fq.QuadPart : 1);
    if (ok) {
        char art[220];
        if (gArt.ok)
            snprintf(art, sizeof art, "MK9's lettering and frame pieces read too (%d and %d letters, %d and %d kerning pairs)", gArt.font[0].glyphs,
                     gArt.font[1].glyphs, gArt.font[0].pairs, gArt.font[1].pairs);
        else
            snprintf(art, sizeof art, "MK9's lettering NOT read, plain tiles (%s)", gArt.why);
        snprintf(gLine, sizeof gLine,
                 "%d arena pictures read from the game's %ls in %.1f ms (package version %d/%d, %d of %d chunks: %u bytes of the file, %u unpacked; "
                 "%d textures, %d not usable); %s%s%s",
                 gSet.count, from, ms, gSet.version, gSet.licensee, gSet.chunksRead, gSet.chunks, (unsigned)gSet.fileBytes, (unsigned)gSet.unpackedBytes,
                 gSet.textures, gSet.skipped, art, tried[0] ? "; before it " : "", tried);
    } else {
        snprintf(gLine, sizeof gLine, "no arena pictures, the stage screen shows names (%s)", tried[0] ? tried : "the game's folder was not found");
    }
    if (ok && gArt.ok) gArtUse.store(&gArt);
    if (ok) gUse.store(&gSet);
    InterlockedExchange(&gState, ok ? kReady : kFailed);
    return 0;
}
}  // namespace

namespace ArenaPics {

// LZO1X as lzo1x_decompress reads it, written as one loop with three ways in: after the stream's start or a match
// without literals (an instruction below 16 is a run of literals), after a run of literals (below 16 is a short match
// far back), after a match's one to three literals (below 16 is a two-byte match).
int LzoDecompress(const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t dstCap) {
    uint32_t ip = 0, op = 0, t = 0;
    int64_t m = 0;
    bool afterLiterals = false, afterMatchLiterals = false;
#define MK9R_IN(n) \
    do { \
        if (srcLen - ip < (uint32_t)(n)) return -1; \
    } while (0)
#define MK9R_OUT(n) \
    do { \
        if (dstCap - op < (uint32_t)(n)) return -1; \
    } while (0)
#define MK9R_LONG(base) \
    do { \
        for (;;) { \
            MK9R_IN(1); \
            if (src[ip]) break; \
            t += 255, ++ip; \
            if (t > 0x01000000u) return -1; \
        } \
        t += (base) + src[ip++]; \
    } while (0)
    MK9R_IN(1);
    if (src[ip] > 17) {
        t = (uint32_t)src[ip++] - 17;
        if (t < 4) {
            afterMatchLiterals = true;
        } else {
            MK9R_IN(t);
            MK9R_OUT(t);
            memcpy(dst + op, src + ip, t);
            ip += t, op += t;
            afterLiterals = true;
        }
    }
    for (;;) {
        if (afterMatchLiterals) {
            afterMatchLiterals = false;
            MK9R_IN(t + 1);
            MK9R_OUT(t);
            do dst[op++] = src[ip++];
            while (--t > 0);
            t = src[ip++];
        } else if (afterLiterals) {
            afterLiterals = false;
            MK9R_IN(1);
            t = src[ip++];
            if (t < 16) {
                MK9R_IN(1);
                m = (int64_t)op - (1 + 0x0800) - (t >> 2) - ((uint32_t)src[ip++] << 2);
                if (m < 0) return -1;
                MK9R_OUT(3);
                dst[op++] = dst[m++], dst[op++] = dst[m++], dst[op++] = dst[m];
                t = src[ip - 2] & 3u;
                afterMatchLiterals = t != 0;
                continue;
            }
        } else {
            MK9R_IN(1);
            t = src[ip++];
            if (t < 16) {
                if (t == 0) MK9R_LONG(15);
                t += 3;
                MK9R_IN(t);
                MK9R_OUT(t);
                memcpy(dst + op, src + ip, t);
                ip += t, op += t;
                afterLiterals = true;
                continue;
            }
        }
        // a match: t is its first byte
        if (t >= 64) {
            MK9R_IN(1);
            m = (int64_t)op - 1 - ((t >> 2) & 7) - ((uint32_t)src[ip++] << 3);
            t = (t >> 5) - 1;
        } else if (t >= 32) {
            t &= 31;
            if (t == 0) MK9R_LONG(31);
            MK9R_IN(2);
            m = (int64_t)op - 1 - ((src[ip] >> 2) + ((uint32_t)src[ip + 1] << 6));
            ip += 2;
        } else if (t >= 16) {
            m = (int64_t)op - ((t & 8) << 11);
            t &= 7;
            if (t == 0) MK9R_LONG(7);
            MK9R_IN(2);
            m -= (src[ip] >> 2) + ((uint32_t)src[ip + 1] << 6);
            ip += 2;
            if (m == (int64_t)op) break;  // the end of the stream
            m -= 0x4000;
        } else {
            MK9R_IN(1);
            m = (int64_t)op - 1 - (t >> 2) - ((uint32_t)src[ip++] << 2);
            if (m < 0) return -1;
            MK9R_OUT(2);
            dst[op++] = dst[m++], dst[op++] = dst[m];
            t = src[ip - 2] & 3u;
            afterMatchLiterals = t != 0;
            continue;
        }
        if (m < 0) return -1;
        MK9R_OUT(t + 2);
        dst[op++] = dst[m++], dst[op++] = dst[m++];
        do dst[op++] = dst[m++];
        while (--t > 0);
        t = src[ip - 2] & 3u;
        afterMatchLiterals = t != 0;
    }
#undef MK9R_IN
#undef MK9R_OUT
#undef MK9R_LONG
    return ip == srcLen ? (int)op : -1;
}

void PictureName(const char* tableName, char* out, int cap) {
    if (cap <= 0) return;
    const char* s = tableName ? tableName : "";
    if (!_strnicmp(s, "BGND_", 5)) s += 5;
    char low[kNameLen];
    int n = 0;
    for (; s[n] && n < kNameLen - 1; ++n) low[n] = s[n] >= 'A' && s[n] <= 'Z' ? (char)(s[n] + 32) : s[n];
    low[n] = 0;
    const char* name = s[n] ? "" : low;  // a name too long for a picture's name has none
    if (!strcmp(low, "kahnscoliseum")) name = "koliseum";
    else if (!strcmp(low, "stthrone")) name = "shangsthroneroom";
    else if (!strcmp(low, "stgardens")) name = "stgardenday";
    snprintf(out, (size_t)cap, "%s", name);
}

Place PlaceOf(PieceId p) { return p >= 0 && p < kPieces ? kPlaces[p] : Place{0, 0, 0, 0}; }

bool Load(const Source& src, Set* out, Art* art) {
    Keep(out, art);
    if (!out->sheet || !src.read || out->stride < kSheetW) {
        snprintf(out->why, sizeof out->why, "nothing to read from, or nowhere to put it");
        if (art) snprintf(art->why, sizeof art->why, "%s", out->why);
        return false;
    }
    for (int y = 0; y < kSheetH; ++y)
        for (int x = 0; x < kSheetW; ++x) out->sheet[(size_t)y * (size_t)out->stride + x] = 0xFF000000u;
    if (art && art->atlas)  // the pieces' quarter: transparent between them
        for (int y = 0; y < kAtlasH / 2; ++y) memset(art->atlas + (size_t)y * kAtlasW + kAtlasW / 2, 0, (size_t)(kAtlasW / 2) * 4);
    Package p(src, out, art);
    return p.Run();
}

bool LoadFile(const wchar_t* path, Set* out, Art* art) {
    FileCtx f{CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    LARGE_INTEGER size{};
    if (f.h == INVALID_HANDLE_VALUE || !GetFileSizeEx(f.h, &size)) {
        const DWORD err = GetLastError();
        if (f.h != INVALID_HANDLE_VALUE) CloseHandle(f.h);
        Keep(out, art);
        snprintf(out->why, sizeof out->why, "the file cannot be opened (error %lu)", (unsigned long)err);
        if (art) snprintf(art->why, sizeof art->why, "%s", out->why);
        return false;
    }
    const Source src{FileRead, &f, (uint64_t)size.QuadPart};
    const bool ok = Load(src, out, art);
    CloseHandle(f.h);
    return ok;
}

int Find(const Set& s, const char* pictureName) {
    if (!pictureName || !*pictureName) return -1;
    for (int i = 0; i < s.count && i < kSlots; ++i)
        if (!strcmp(s.name[i], pictureName)) return i;
    return -1;
}

void Request() {
    if (InterlockedCompareExchange(&gState, kLoading, kNotAsked) != kNotAsked) return;
    gThread = CreateThread(nullptr, 0, ReadThread, nullptr, 0, nullptr);
    if (!gThread) {
        snprintf(gLine, sizeof gLine, "no arena pictures, the stage screen shows names (the reading thread did not start)");
        InterlockedExchange(&gState, kFailed);
    }
}

int State() { return gState; }
bool Usable() { return gWanted && !gRefused && gUse.load() != nullptr; }
int Count() {
    const Set* s = gUse.load();
    return s ? s->count : 0;
}
int SlotOf(const char* tableName) {
    const Set* s = gUse.load();
    if (!s) return -1;
    char name[kNameLen];
    PictureName(tableName, name, sizeof name);
    return Find(*s, name);
}
int SlotOfPicture(const char* pictureName) {
    const Set* s = gUse.load();
    return s ? Find(*s, pictureName) : -1;
}
uint32_t* Atlas() {  // the set's sheet where it is an atlas' corner (the mod's own always is; a host test's may be a bare sheet)
    const Set* s = gUse.load();
    return s && s->stride == kAtlasW ? s->sheet : nullptr;
}
const Art* ArtOf() {
    const Art* a = gArtUse.load();
    return a && a->ok && gLookWanted && Usable() ? a : nullptr;
}
void SetWanted(bool on) { InterlockedExchange(&gWanted, on ? 1 : 0); }
void SetLookWanted(bool on) { InterlockedExchange(&gLookWanted, on ? 1 : 0); }
void Refuse(const char* why) {
    if (InterlockedExchange(&gRefused, 1)) return;
    snprintf(gRefusal, sizeof gRefusal, "%s", why ? why : "");
}
void Describe(char* out, int n) {
    if (n <= 0) return;
    const LONG st = gState;
    if (st == kNotAsked) snprintf(out, (size_t)n, "arena pictures not asked for yet");
    else if (st == kLoading) snprintf(out, (size_t)n, "arena pictures are being read");
    else if (gRefused) snprintf(out, (size_t)n, "%s; not shown: %s", gLine, gRefusal);
    else snprintf(out, (size_t)n, "%s%s%s", gLine, gWanted ? "" : "; switched off ('stagepics 0')", gLookWanted ? "" : "; MK9's look switched off ('stagelook 0')");
}
void Shutdown() {
    if (!gThread) return;
    // The thread runs for some tens of milliseconds. Should it not have ended, its memory stays (it is still writing).
    const bool ended = WaitForSingleObject(gThread, 5000) == WAIT_OBJECT_0;
    CloseHandle(gThread);
    gThread = nullptr;
    gArtUse.store(nullptr);
    gUse.store(nullptr);
    if (!ended) return;
    if (gLetters) VirtualFree(gLetters, 0, MEM_RELEASE);
    if (gAtlas) VirtualFree(gAtlas, 0, MEM_RELEASE);
    gLetters = gAtlas = nullptr;
}
void Adopt(const Set* s, const Art* art) {
    gArtUse.store(s && art && art->ok ? art : nullptr);
    gUse.store(s);
    InterlockedExchange(&gState, s ? kReady : kNotAsked);
    InterlockedExchange(&gRefused, 0);
    InterlockedExchange(&gWanted, 1);
    InterlockedExchange(&gLookWanted, 1);
}

}  // namespace ArenaPics
