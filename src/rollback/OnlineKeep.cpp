#include "OnlineKeep.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../game/MKHeap.h"
#include "../net/OnlineMode.h"
#include "FreeQuarantine.h"

namespace {
constexpr uintptr_t kHeap5Ptr = 0x00ED5400;
constexpr uint32_t kHeapClass = 0x00C66A3C;
// Class 0x00C66A3C header (a 0x3C-byte GMalloc object, the "heapobj" span): +0x24 reservation, +0x28 bytes used,
// +0x30 spinlock, +0x34 free-tree root.
constexpr uint32_t kHeaderSize = 0x3C, kLockOff = 0x30;
constexpr uint32_t kMaxKeep = 0x400000;
constexpr uint32_t kMeasureFirst = 12, kMeasureEvery = 600;  // measured loads per session (a 1 MB copy and compare each)

int gMode = 1;

struct SaveRec {
    uint32_t gf, ops, heap;
    bool valid;
};
constexpr int kSaves = 16;  // more than the snapshot ring (12)
SaveRec gSaves[kSaves] = {};
uint32_t gSaveOps = 0;
bool gSaveCounted = false;

struct Range {
    uintptr_t base;
    uint32_t size;
};
constexpr int kMaxRanges = 16;
Range gKeep[kMaxRanges];
int gKeepN = 0;
const char* gSource = "";
std::vector<std::pair<uintptr_t, uintptr_t>> gHoles;  // snapshot spans inside the kept memory: restored as saved
std::vector<uint8_t> gLive;                            // the kept memory before the load
bool gPending = false, gPendingKeep = false;
uint32_t gPendingOps = 0;

enum Refusal { kNotCounted, kNoRecord, kHeapTraffic, kHeaderChanged, kNoMemory, kNoConnection, kTrafficDuringLoad, kReasons };
const char* const kReasonText[kReasons] = {
    "the MK heap hooks are not installed (heap #5 allocations not counted)",
    "no heap #5 record for the snapshot frame",
    "heap #5 allocated or freed since the save",
    "heap #5's header differs from the saved one",
    "heap #5's memory not found inside the master image",
    "MK9's connection not inside heap #5's memory",
    "heap #5 allocated or freed during the load",
};
uint32_t gRefused[kReasons] = {};
uint32_t gLoads = 0, gEligible = 0, gKept = 0, gMeasured = 0, gDiffLogs = 0;
uint64_t gDwords = 0, gDwordsConn = 0;  // dwords the restore changed in the kept/measured memory, and in the connection

uint32_t Read32(uintptr_t a) {
    uint32_t v = 0;
    CrashLog::SafeRead32(a, &v);
    return v;
}

bool Refuse(Refusal r, uint32_t S, uint32_t count = 0) {
    if (gRefused[r]++ == 0) {
        if (count)
            Logger::Info("ONLINEKEEP S=%u: MK9's online memory (MK heap #5) rewound with this load: %s (%u allocation(s)/free(s)); "
                         "logged once per reason and session",
                         S, kReasonText[r], count);
        else
            Logger::Info("ONLINEKEEP S=%u: MK9's online memory (MK heap #5) rewound with this load: %s; logged once per reason and session",
                         S, kReasonText[r]);
    }
    return false;
}

// Heap #5's memory: its segments in the system segment registry (MKHeap::HeapSegments) if it has any there, else the
// region its header describes: [free-tree root +0x34, + reservation +0x24). The root is the region's first chunk only
// while it is the big chunk allocations are carved from (every log so far: root 0x119AFFD0, 70 of 1024 KB used, the
// connection near the top); a root that moved to a smaller free chunk would put the range past the heap's end, into
// master-heap memory (review 2026-11-01), so the root chunk must hold at least half the reservation. Clipped to the
// master image's ranges (only those are restored), and never reaching another MK heap's first chunk.
int Heap5Ranges(uint32_t h, Range* out, int max) {
    MKHeap::Region seg[kMaxRanges];
    int ns = MKHeap::HeapSegments(h, seg, kMaxRanges);
    gSource = "segment registry";
    if (ns <= 0) {
        const uint32_t root = Read32(h + 0x34), res = Read32(h + 0x24);
        if (!root || !res || res > kMaxKeep) return 0;
        const uint32_t rootSize = Read32(root + 4) & ~7u;  // free chunk {+4 size|flags}
        if (rootSize < res / 2 || rootSize > res) return 0;
        seg[0] = {root, res};
        ns = 1;
        gSource = "header";
    }
    MKHeap::Region mr[8];
    const int nmr = MKHeap::MasterRanges(mr, 8);
    int n = 0;
    uint64_t total = 0;
    for (int i = 0; i < ns; ++i)
        for (int j = 0; j < nmr && n < max; ++j) {
            const uintptr_t lo = std::max(seg[i].base, mr[j].base);
            const uintptr_t hi = std::min(seg[i].base + seg[i].size, mr[j].base + mr[j].size);
            if (lo >= hi) continue;
            out[n++] = {lo, (uint32_t)(hi - lo)};
            total += hi - lo;
        }
    if (!n || total > kMaxKeep) return 0;
    for (int k = 0; k < 4; ++k) {  // the master and sub-heaps #2-#4
        const uint32_t other = Read32(0x00ED53F0 + 4 * k);
        const uint32_t root = other && other != h ? Read32(other + 0x34) : 0;
        for (int i = 0; i < n && root; ++i)
            if (root >= out[i].base && root < out[i].base + out[i].size) return 0;
    }
    return n;
}

bool Kept(uintptr_t a, uint32_t size) {
    for (int i = 0; i < gKeepN; ++i)
        if (a >= gKeep[i].base && a + size <= gKeep[i].base + gKeep[i].size) return true;
    return false;
}

// What the restore changed in [lo, hi) (restored bytes now, live bytes in src); written back when keeping.
struct Diff {
    uint32_t dwords, inConn;
    uintptr_t firstOut, lastOut;
    uintptr_t sample[3];
    uint32_t sampleOld[3], sampleNew[3];
    int samples;
};
void Put(uintptr_t lo, uintptr_t hi, const uint8_t* src, uintptr_t conn, uint32_t connSize, bool write, Diff& d) {
    uint8_t* dst = reinterpret_cast<uint8_t*>(lo);
    const size_t n = hi - lo;
    if (!memcmp(dst, src, n)) return;
    for (size_t o = 0; o + 4 <= n; o += 4) {
        uint32_t was, now;
        memcpy(&was, dst + o, 4);
        memcpy(&now, src + o, 4);
        if (was == now) continue;
        ++d.dwords;
        const uintptr_t a = lo + o;
        if (conn && a >= conn && a < conn + connSize) {
            ++d.inConn;
            continue;
        }
        if (!d.firstOut) d.firstOut = a;
        d.lastOut = a;
        if (d.samples < 3) {
            d.sample[d.samples] = a;
            d.sampleOld[d.samples] = was;
            d.sampleNew[d.samples++] = now;
        }
    }
    if (write) memcpy(dst, src, n);
}
}  // namespace

namespace OnlineKeep {

void BeforeSave() { gSaveCounted = gMode && FreeQuarantine::Heap5Ops(&gSaveOps); }

void OnSave(uint32_t gf) {
    SaveRec& r = gSaves[gf % kSaves];
    r.valid = false;
    uint32_t ops = 0;
    // An allocation or free in heap #5 while the save ran (another thread): the snapshot may hold either side of it.
    if (!gSaveCounted || !FreeQuarantine::Heap5Ops(&ops) || ops != gSaveOps) return;
    r = {gf, ops, Read32(kHeap5Ptr), true};
}

bool BeforeLoad(const Snapshot::StateAB& snap) {
    gPending = false;
    if (!gMode || (gMode != 3 && !Online::SkipNative())) return false;
    ++gLoads;
    const bool keep = gMode >= 2;
    const uint32_t S = snap.gameFrame;
    const uint32_t h = Read32(kHeap5Ptr);
    uint32_t ops = 0;
    if (!h || Read32(h) != kHeapClass || !FreeQuarantine::Heap5Ops(&ops)) return Refuse(kNotCounted, S);
    const SaveRec& rec = gSaves[S % kSaves];
    if (!rec.valid || rec.gf != S || rec.heap != h) return Refuse(kNoRecord, S);
    if (ops != rec.ops) return Refuse(kHeapTraffic, S, ops - rec.ops);
    bool same = false;
    for (size_t i = 0; i < snap.spans.size() && i < snap.bytes.size(); ++i) {
        const Snapshot::Span& s = snap.spans[i];
        if (s.base != h || strcmp(s.name, "heapobj") || snap.bytes[i].size() < kHeaderSize) continue;
        const uint8_t* saved = snap.bytes[i].data();
        const uint8_t* live = reinterpret_cast<const uint8_t*>(h);
        same = !memcmp(saved, live, kLockOff) && !memcmp(saved + kLockOff + 4, live + kLockOff + 4, kHeaderSize - kLockOff - 4);
        break;
    }
    if (!same) return Refuse(kHeaderChanged, S);
    gKeepN = Heap5Ranges(h, gKeep, kMaxRanges);
    if (gKeepN <= 0) return Refuse(kNoMemory, S);
    if (gMode != 3) {
        uint32_t connSize = 0;
        const uintptr_t conn = Online::ConnectionAddress(&connSize);
        if (!conn || !Kept(conn, connSize)) return Refuse(kNoConnection, S);
    }
    ++gEligible;
    if (!keep && gEligible > kMeasureFirst && gEligible % kMeasureEvery) return false;  // measuring: a sample of the loads
    gHoles.clear();
    for (const Snapshot::Span& s : snap.spans) {
        if (!s.size || !strcmp(s.name, "mkmaster")) continue;
        for (int i = 0; i < gKeepN; ++i) {
            const uintptr_t lo = std::max(s.base, gKeep[i].base), hi = std::min(s.base + s.size, gKeep[i].base + gKeep[i].size);
            if (lo < hi) gHoles.push_back({lo, hi});
        }
    }
    std::sort(gHoles.begin(), gHoles.end());
    size_t total = 0;
    for (int i = 0; i < gKeepN; ++i) total += gKeep[i].size;
    gLive.resize(total);
    size_t at = 0;
    for (int i = 0; i < gKeepN; ++i) {
        memcpy(gLive.data() + at, reinterpret_cast<const void*>(gKeep[i].base), gKeep[i].size);
        at += gKeep[i].size;
    }
    gPending = true;
    gPendingKeep = keep;
    gPendingOps = ops;
    return keep;
}

void AfterLoad(uint32_t S, bool restored) {
    if (!gPending) return;
    gPending = false;
    if (!restored) return;  // refused before writing anything: the memory is as captured
    uint32_t ops = 0;
    // Another thread allocated or freed in heap #5 while the image was restored (its spinlock was not held after all):
    // neither copy is the heap's state.
    if (!FreeQuarantine::Heap5Ops(&ops) || ops != gPendingOps) {
        Refuse(kTrafficDuringLoad, S, ops - gPendingOps);
        return;
    }
    uint32_t connSize = 0;
    const uintptr_t conn = Online::ConnectionAddress(&connSize);
    Diff d{};
    size_t at = 0;
    for (int i = 0; i < gKeepN; ++i) {
        const Range& r = gKeep[i];
        const uint8_t* live = gLive.data() + at;
        const uintptr_t end = r.base + r.size;
        uintptr_t p = r.base;
        for (const auto& hole : gHoles) {
            if (hole.second <= p || hole.first >= end) continue;
            if (hole.first > p) Put(p, hole.first, live + (p - r.base), conn, connSize, gPendingKeep, d);
            p = std::max(p, hole.second);
        }
        if (p < end) Put(p, end, live + (p - r.base), conn, connSize, gPendingKeep, d);
        at += r.size;
    }
    if (gPendingKeep) ++gKept;
    else ++gMeasured;
    gDwords += d.dwords;
    gDwordsConn += d.inConn;
    if (gDiffLogs < kMeasureFirst || (gKept + gMeasured) % kMeasureEvery == 0) {
        ++gDiffLogs;
        uint32_t kb = 0;
        for (int i = 0; i < gKeepN; ++i) kb += gKeep[i].size >> 10;
        char out[160] = "";
        if (d.dwords > d.inConn) {
            int k = snprintf(out, sizeof out, "; outside it %08X..%08X:", (unsigned)d.firstOut, (unsigned)d.lastOut);
            for (int i = 0; i < d.samples && k > 0 && k < (int)sizeof out; ++i)
                k += snprintf(out + k, sizeof out - k, " %08X %08X->%08X", (unsigned)d.sample[i], d.sampleOld[i], d.sampleNew[i]);
        }
        Logger::Info("ONLINEKEEP S=%u: MK9's online memory (heap #5, %u KB in %d range(s) from the %s, %u snapshot span(s) inside it "
                     "restored as saved) %s: the restore changed %u dword(s), %u in the connection %08X%s",
                     S, kb, gKeepN, gSource, (unsigned)gHoles.size(), gPendingKeep ? "kept live" : "measured, left rewound", d.dwords,
                     d.inConn, (unsigned)conn, out);
    }
}

void OnSessionEnd() {
    if (gLoads) {
        char why[400] = "";
        int k = 0;
        for (int r = 0; r < kReasons; ++r)
            if (gRefused[r] && k >= 0 && k < (int)sizeof why)
                k += snprintf(why + k, sizeof why - k, "%s%u x %s", k ? ", " : "", gRefused[r], kReasonText[r]);
        Logger::Info("ONLINEKEEP session (mode %d): %u load(s), %u passed the heap checks; %u kept live, %u measured; the restore "
                     "changed %llu dword(s) there, %llu in MK9's connection%s%s",
                     gMode, gLoads, gEligible, gKept, gMeasured, (unsigned long long)gDwords, (unsigned long long)gDwordsConn,
                     k ? "; refused: " : "", why);
    }
    gLoads = gEligible = gKept = gMeasured = gDiffLogs = 0;
    gDwords = gDwordsConn = 0;
    memset(gRefused, 0, sizeof gRefused);
    memset(gSaves, 0, sizeof gSaves);
    gPending = false;
}

void SetMode(int mode) {
    gMode = mode < 0 ? 0 : mode > 3 ? 3 : mode;
    memset(gSaves, 0, sizeof gSaves);
}
int Mode() { return gMode; }

}  // namespace OnlineKeep
