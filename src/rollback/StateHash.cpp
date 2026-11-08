#include "StateHash.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "../game/MK9Game.h"
#include "../game/MKHeap.h"

namespace {
constexpr uintptr_t kRngState = 0x00E7C054;
constexpr uint32_t kLcgMul = 0x0BB38435, kLcgAdd = 0x3619636B;  // 0x005F5080
constexpr int kMaxRngSteps = 20000;
constexpr uint32_t kCharObjSize = 0xC580;  // MKFGCharacterObj allocation size (docs/desync_rng_static.md)
constexpr uintptr_t kRngStateB = 0x00E5352C;  // UE3 GSRandSeed, unsynced cosmetic stream (+ a few script natives)
constexpr uint32_t kPadSize = 0x60C;       // the whole pad object (hardware stride), incl. the ring index at +0x5F8

struct Region {
    const char* name;
    uintptr_t base;
    uint32_t size;
};

uint32_t gPrevRng = 0;
bool gHavePrevRng = false;
std::vector<uint8_t> gScratch;

uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

bool Readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (p < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uintptr_t CharObj(int player) {
    const uintptr_t info = MK9Game::Get().playerInfoBase + player * MK9Game::kPlayerInfoStride;
    const uintptr_t obj = Read32(info + 0x1C);
    if (obj == 0 || obj == 0xDEADBEEF || Read32(info + 0x20) == 0xFFFFFFFF) return 0;
    return Read32(obj + 4) == Read32(info + 0x20) ? obj : 0;
}

uintptr_t PadPtr(int player) {
    return Read32(MK9Game::Get().playerInfoBase + player * MK9Game::kPlayerInfoStride + 0xB40);
}

int Regions(Region* r) {
    const uintptr_t gameInfo = Read32(MK9Game::Get().gameInfoPtr);
    int n = 0;
    r[n++] = {"gameinfo", gameInfo, 0x20C};  // +0x6C/+0x70 masked in Copy()
    for (int i = 0; i < 4; ++i) {
        static const char* names[4] = {"pinfo0", "pinfo1", "pinfo2", "pinfo3"};
        r[n++] = {names[i], MK9Game::Get().playerInfoBase + i * MK9Game::kPlayerInfoStride, MK9Game::kPlayerInfoStride};
    }
    r[n++] = {"char0", CharObj(0), kCharObjSize};
    r[n++] = {"char1", CharObj(1), kCharObjSize};
    r[n++] = {"pad0", PadPtr(0), kPadSize};
    r[n++] = {"pad1", PadPtr(1), kPadSize};
    r[n++] = {"rng", kRngState, 4};
    r[n++] = {"rngB", kRngStateB, 4};
    r[n++] = {"gamefull", gameInfo, 0x82E0};  // HostFrame/GameFrame masked in Copy()
    static uintptr_t heapForObj = 0;
    static MKHeap::Region heap{};
    if (CharObj(0) != heapForObj) {
        heapForObj = CharObj(0);
        if (!heapForObj || !MKHeap::Discover(&heap)) heap = {};
    }
    r[n++] = {"mkheap", heap.base, heap.size};
    return n;
}

// Copy a region into scratch, masking the absolute frame counters. Returns
// nullptr if the region is unreadable.
const uint8_t* Copy(const Region& r, uintptr_t gameInfo) {
    if (!r.base) return nullptr;
    static uintptr_t validatedBase[16];
    static uint32_t validatedSize[16];
    static int slots = 0;
    bool known = false;
    for (int i = 0; i < slots; ++i) known |= validatedBase[i] == r.base && validatedSize[i] >= r.size;
    if (!known) {
        if (!Readable(r.base, r.size)) return nullptr;
        if (slots < 16) {
            validatedBase[slots] = r.base;
            validatedSize[slots++] = r.size;
        }
    }
    gScratch.resize(r.size);
    memcpy(gScratch.data(), reinterpret_cast<const void*>(r.base), r.size);
    if (r.base == gameInfo) {
        memset(gScratch.data() + 0x6C, 0, 8);  // HostFrame, GameFrame
    }
    return gScratch.data();
}

inline uint32_t Rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
}  // namespace

namespace StateHash {

uint32_t XXH32(const void* data, size_t len, uint32_t seed) {
    const uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    uint32_t h;
    auto rd = [](const uint8_t* q) { uint32_t v; memcpy(&v, q, 4); return v; };
    if (len >= 16) {
        uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* limit = end - 16;
        do {
            v1 = Rotl(v1 + rd(p) * P2, 13) * P1; p += 4;
            v2 = Rotl(v2 + rd(p) * P2, 13) * P1; p += 4;
            v3 = Rotl(v3 + rd(p) * P2, 13) * P1; p += 4;
            v4 = Rotl(v4 + rd(p) * P2, 13) * P1; p += 4;
        } while (p <= limit);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
    } else {
        h = seed + P5;
    }
    h += (uint32_t)len;
    while (p + 4 <= end) { h = Rotl(h + rd(p) * P3, 17) * P4; p += 4; }
    while (p < end) { h = Rotl(h + (*p) * P5, 11) * P1; ++p; }
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

void Compute(Result* out) {
    Region r[kMaxRegions];
    const int n = Regions(r);
    const uintptr_t gameInfo = Read32(MK9Game::Get().gameInfoPtr);
    out->count = n;
    out->combined = 0;
    for (int i = 0; i < n; ++i) {
        out->names[i] = r[i].name;
        const uint8_t* bytes = Copy(r[i], gameInfo);
        out->hashes[i] = bytes ? XXH32(bytes, r[i].size, 0) : 0;
        out->combined = XXH32(&out->hashes[i], 4, out->combined);
    }

    const uint32_t rng = Read32(kRngState);
    out->rngState = rng;
    out->rngDraws = -1;
    if (gHavePrevRng) {
        uint32_t x = gPrevRng;
        for (int k = 0; k <= kMaxRngSteps; ++k) {
            if (x == rng) {
                out->rngDraws = k;
                break;
            }
            x = x * kLcgMul + kLcgAdd;
        }
    }
    gPrevRng = rng;
    gHavePrevRng = true;

    static uint32_t prevIds = 0;
    const uint32_t ids = Read32(MKHeap::kObjectIdCounter);
    out->objectsCreated = prevIds ? ids - prevIds : 0;
    prevIds = ids;
    out->scriptDepth = (int32_t)Read32(MKHeap::kScriptDepth);
}

bool Dump(const wchar_t* dir, const char* label, uint32_t gameFrame) {
    wchar_t folder[MAX_PATH], bin[MAX_PATH], txt[MAX_PATH];
    swprintf(folder, MAX_PATH, L"%ls\\dumps", dir);
    CreateDirectoryW(folder, nullptr);
    swprintf(bin, MAX_PATH, L"%ls\\%hs.bin", folder, label);
    swprintf(txt, MAX_PATH, L"%ls\\%hs.txt", folder, label);
    FILE* fb = _wfopen(bin, L"wb");
    FILE* ft = _wfopen(txt, L"w");
    if (!fb || !ft) {
        if (fb) fclose(fb);
        if (ft) fclose(ft);
        return false;
    }
    Region r[kMaxRegions];
    const int n = Regions(r);
    const uintptr_t gameInfo = Read32(MK9Game::Get().gameInfoPtr);
    fprintf(ft, "label=%s gf=%u\n", label, gameFrame);
    long offset = 0;
    for (int i = 0; i < n; ++i) {
        const uint8_t* bytes = Copy(r[i], gameInfo);
        const uint32_t size = bytes ? r[i].size : 0;
        if (bytes) fwrite(bytes, 1, size, fb);
        fprintf(ft, "%s base=%08X size=%u fileoff=%ld\n", r[i].name, (unsigned)r[i].base, size, offset);
        offset += size;
    }
    fclose(fb);
    fclose(ft);
    return true;
}

}  // namespace StateHash
