#include "ParallelMem.h"

#include <emmintrin.h>
#include <windows.h>

#include <cstring>

#include "../debug/Logger.h"

namespace ParallelMem {
namespace {

void ScanDwords(const uint32_t* d, uint32_t n, const uint32_t* set, uint32_t cap, uint32_t lo, uint32_t hi,
                std::vector<uint32_t>& hits) {
    auto probe = [&](uint32_t v) {
        for (uint32_t i = (v >> 3) * 2654435761u & (cap - 1); set[i]; i = (i + 1) & (cap - 1))
            if (set[i] == v) {
                hits.push_back(v);
                break;
            }
    };
    // Four dwords at a time (SSE2): only aligned values inside [lo, hi] are probed.
    const __m128i bias = _mm_set1_epi32((int)0x80000000u), vlo = _mm_set1_epi32((int)lo);
    const __m128i vspan = _mm_set1_epi32((int)((hi - lo) ^ 0x80000000u)), seven = _mm_set1_epi32(7), zero = _mm_setzero_si128();
    uint32_t k = 0;
    for (; k + 4 <= n; k += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + k));
        const __m128i outside = _mm_cmpgt_epi32(_mm_xor_si128(_mm_sub_epi32(v, vlo), bias), vspan);  // v - lo > hi - lo
        const __m128i aligned = _mm_cmpeq_epi32(_mm_and_si128(v, seven), zero);
        const int m = _mm_movemask_ps(_mm_castsi128_ps(_mm_andnot_si128(outside, aligned)));
        if (!m) continue;
        for (int j = 0; j < 4; ++j)
            if (m & (1 << j)) probe(d[k + j]);
    }
    for (; k < n; ++k) {
        const uint32_t v = d[k];
        if (v >= lo && v <= hi && !(v & 7)) probe(v);
    }
}

constexpr LONG kNoChunk = 0x40000000;  // 'next' between passes: every grab is past the last chunk
constexpr uint32_t kChunk = 128 * 1024;
constexpr int kMaxHelpers = 3;
constexpr size_t kStreamMin = 64 * 1024;  // CopyToSlot: smaller copies use memcpy (the sfence and head/tail cost more)

// memcpy with non-temporal stores for the 16-byte-aligned middle (head and tail through memcpy). The bytes written are
// memcpy's. The caller issues _mm_sfence() before it reports the copy done to another thread.
void StreamCopy(void* dst, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    size_t head = (16 - (reinterpret_cast<uintptr_t>(d) & 15)) & 15;
    if (head > n) head = n;
    if (head) {
        memcpy(d, s, head);
        d += head;
        s += head;
        n -= head;
    }
    for (; n >= 64; n -= 64, d += 64, s += 64) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
        const __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
        _mm_stream_si128(reinterpret_cast<__m128i*>(d), a);
        _mm_stream_si128(reinterpret_cast<__m128i*>(d + 16), b);
        _mm_stream_si128(reinterpret_cast<__m128i*>(d + 32), c);
        _mm_stream_si128(reinterpret_cast<__m128i*>(d + 48), e);
    }
    if (n) memcpy(d, s, n);
}

struct Chunk {
    uint8_t* dst;  // null: scan
    const uint8_t* src;
    uint32_t bytes;
};
struct Job {
    const uint32_t* set = nullptr;
    uint32_t cap = 0, lo = 0, hi = 0;
    bool stream = false;  // copy chunks with non-temporal stores (CopyToSlot); written before the pass is published
    std::vector<Chunk> chunks;
    volatile LONG count = 0;
    volatile LONG next = kNoChunk;
    volatile LONG inFlight = 0;  // threads between taking a chunk index and finishing that chunk
};
Job gJob;
// Takes chunks until none is left. A chunk is only touched after its index was handed out below 'count', and such
// indices exist only between the publication of a pass and the end of its wait (Run), so a helper that wakes late
// (after the pass) or early (while the next one is written) takes an index >= count and touches nothing.
void RunChunks(std::vector<uint32_t>& hits) {
    for (;;) {
        InterlockedIncrement(&gJob.inFlight);
        const uint32_t i = (uint32_t)InterlockedIncrement(&gJob.next) - 1;
        if (i >= (uint32_t)gJob.count) {
            InterlockedDecrement(&gJob.inFlight);
            return;
        }
        const Chunk& c = gJob.chunks[i];
        if (c.dst && gJob.stream) {
            StreamCopy(c.dst, c.src, c.bytes);
            _mm_sfence();  // this thread's non-temporal stores are globally visible before the chunk counts as done
        } else if (c.dst) {
            memcpy(c.dst, c.src, c.bytes);
        } else {
            ScanDwords(reinterpret_cast<const uint32_t*>(c.src), c.bytes / 4, gJob.set, gJob.cap, gJob.lo, gJob.hi, hits);
        }
        InterlockedDecrement(&gJob.inFlight);
    }
}
struct Helper {
    HANDLE wake = nullptr, thread = nullptr;
    std::vector<uint32_t> hits;
};
Helper gHelpers[kMaxHelpers];
int gStarted = 0;
int gWanted = -1;  // -1: from the processor count at the first pass
int gLast = 0;
volatile LONG gStop = 0;
volatile LONG gBusy = 0;
DWORD gOwner = 0;  // the thread of the first pass (the game thread)
DWORD WINAPI HelperRun(void* p) {
    Helper* h = static_cast<Helper*>(p);
    while (WaitForSingleObject(h->wake, INFINITE) == WAIT_OBJECT_0 && !gStop) RunChunks(h->hits);
    return 0;
}
int Helpers() {
    if (gWanted < 0) {
        // Half the logical processors minus the game thread (4 threads: 1 helper; 8 or more: 3), leaving the render
        // thread and the rest of the game theirs.
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const int n = (int)si.dwNumberOfProcessors / 2 - 1;
        gWanted = n < 0 ? 0 : n > kMaxHelpers ? kMaxHelpers : n;
        Logger::Info("MEMTHREADS: %d helper thread(s) for the big copies and scans of saves and loads (%lu logical processors)", gWanted,
                     si.dwNumberOfProcessors);
    }
    while (gStarted < gWanted && !gStop) {
        Helper& h = gHelpers[gStarted];
        h.hits.reserve(256);
        h.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (h.wake) h.thread = CreateThread(nullptr, 0, HelperRun, &h, 0, nullptr);
        if (!h.thread) {
            if (h.wake) CloseHandle(h.wake);
            h.wake = nullptr;
            gWanted = gStarted;
            break;
        }
        SetThreadPriority(h.thread, THREAD_PRIORITY_ABOVE_NORMAL);  // the game thread waits for it
        ++gStarted;
    }
    return gWanted < gStarted ? gWanted : gStarted;
}
// Claims the pool for one pass on the owner thread; false: the caller does the pass alone.
bool Claim() {
    const DWORD me = GetCurrentThreadId();
    if (!gOwner) gOwner = me;
    if (me != gOwner || gStop) return false;
    return InterlockedCompareExchange(&gBusy, 1, 0) == 0;
}
// Runs the published chunks with 'helpers' helpers; the caller's hits go to 'hits', the helpers' are appended.
void Run(int helpers, std::vector<uint32_t>* hits) {
    // Every started helper's results are cleared and collected, not only the ones woken now: one woken for an earlier
    // pass (and slow to run) can take chunks of this one. No helper is inside a chunk at this point.
    for (int i = 0; i < gStarted; ++i) gHelpers[i].hits.clear();
    InterlockedExchange(&gJob.count, (LONG)gJob.chunks.size());
    InterlockedExchange(&gJob.next, 0);
    for (int i = 0; i < helpers; ++i) SetEvent(gHelpers[i].wake);
    static std::vector<uint32_t> none;
    RunChunks(hits ? *hits : none);
    while (InterlockedCompareExchange(&gJob.inFlight, 0, 0) != 0) YieldProcessor();
    InterlockedExchange(&gJob.next, kNoChunk);
    if (hits)
        for (int i = 0; i < gStarted; ++i) hits->insert(hits->end(), gHelpers[i].hits.begin(), gHelpers[i].hits.end());
    gLast = helpers;
}

}  // namespace

void SetHelpers(int n) {
    gWanted = n < 0 ? -1 : n > kMaxHelpers ? kMaxHelpers : n;
    Logger::Info("MEMTHREADS %d: helpers %s", n, n < 0 ? "from the processor count" : "fixed");
}
int LastHelpers() { return gLast; }

void Stop() {
    InterlockedExchange(&gStop, 1);
    HANDLE h[kMaxHelpers];
    int n = 0;
    for (int i = 0; i < gStarted; ++i)
        if (gHelpers[i].thread) {
            SetEvent(gHelpers[i].wake);
            h[n++] = gHelpers[i].thread;
        }
    if (n) WaitForMultipleObjects(n, h, TRUE, 3000);
}

namespace {
// One copy pass; 'stream': non-temporal stores (CopyToSlot) from kStreamMin up.
void CopyPass(void* dst, const void* src, size_t bytes, bool stream) {
    stream = stream && bytes >= kStreamMin;
    auto alone = [&] {
        if (stream) {
            StreamCopy(dst, src, bytes);
            _mm_sfence();
        } else {
            memcpy(dst, src, bytes);
        }
    };
    if (bytes < 2 * kChunk || !Claim()) {
        alone();
        return;
    }
    const int helpers = Helpers();
    if (helpers <= 0) {
        alone();
        gLast = 0;
        InterlockedExchange(&gBusy, 0);
        return;
    }
    gJob.stream = stream;
    gJob.chunks.clear();
    for (size_t o = 0; o < bytes; o += kChunk)
        gJob.chunks.push_back({static_cast<uint8_t*>(dst) + o, static_cast<const uint8_t*>(src) + o,
                               (uint32_t)(bytes - o < kChunk ? bytes - o : kChunk)});
    Run(helpers, nullptr);
    gJob.stream = false;
    InterlockedExchange(&gBusy, 0);
}
}  // namespace

void Copy(void* dst, const void* src, size_t bytes) { CopyPass(dst, src, bytes, false); }
bool gSlotStreaming = true;  // 'slotnt 0|1' (rc25 A/B): CopyToSlot with non-temporal stores, or as Copy
void SetSlotStreaming(bool on) {
    gSlotStreaming = on;
    Logger::Info("SLOTNT %d: copies into save slots use %s", on ? 1 : 0, on ? "non-temporal stores" : "memcpy");
}
void CopyToSlot(void* dst, const void* src, size_t bytes) { CopyPass(dst, src, bytes, gSlotStreaming); }

void ScanForValues(const std::vector<std::pair<uintptr_t, uint32_t>>& ranges, const uint32_t* set, uint32_t cap, uint32_t lo,
                   uint32_t hi, std::vector<uint32_t>& hits) {
    uint64_t total = 0;
    for (const auto& r : ranges) total += r.second;
    const bool claimed = total >= 2 * kChunk && Claim();
    const int helpers = claimed ? Helpers() : 0;
    if (helpers <= 0) {
        for (const auto& [base, size] : ranges) ScanDwords(reinterpret_cast<const uint32_t*>(base), size / 4, set, cap, lo, hi, hits);
        gLast = 0;
        if (claimed) InterlockedExchange(&gBusy, 0);
        return;
    }
    gJob.set = set;
    gJob.cap = cap;
    gJob.lo = lo;
    gJob.hi = hi;
    gJob.chunks.clear();
    for (const auto& [base, size] : ranges) {
        const uint32_t n = size & ~3u;  // whole dwords, as the single-threaded scan
        for (uint32_t o = 0; o < n; o += kChunk)
            gJob.chunks.push_back({nullptr, reinterpret_cast<const uint8_t*>(base) + o, n - o < kChunk ? n - o : kChunk});
    }
    Run(helpers, &hits);
    InterlockedExchange(&gBusy, 0);
}

}  // namespace ParallelMem
