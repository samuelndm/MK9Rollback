// memscan: hash every writable committed 4 KiB page of a process (read-only access).
//   memscan scan <pid> <out.bin>
//   memscan diff <paused1> <paused2> <afterStep> <paused3>  -> pages changed by the step but not by noise
// Output .bin: header "MKSC", u32 count, then {u32 page, u32 hash, u32 protect, u32 type} records.
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct Rec {
    uint32_t page, hash, protect, type;
};

uint32_t Rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
uint32_t Hash4K(const uint8_t* p) {  // xxHash32-style over 4096 bytes, seed 0
    const uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u;
    uint32_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0u - P1;
    for (int i = 0; i < 4096; i += 16) {
        uint32_t a, b, c, d;
        memcpy(&a, p + i, 4); memcpy(&b, p + i + 4, 4); memcpy(&c, p + i + 8, 4); memcpy(&d, p + i + 12, 4);
        v1 = Rotl(v1 + a * P2, 13) * P1; v2 = Rotl(v2 + b * P2, 13) * P1;
        v3 = Rotl(v3 + c * P2, 13) * P1; v4 = Rotl(v4 + d * P2, 13) * P1;
    }
    uint32_t h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18) + 4096;
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

int Scan(DWORD pid, const char* out) {
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) { fprintf(stderr, "OpenProcess %lu\n", GetLastError()); return 1; }
    std::vector<Rec> recs;
    std::vector<uint8_t> buf;
    uint64_t addr = 0x10000, bytes = 0;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD t0 = GetTickCount();
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x80000000ull && VirtualQueryEx(p, (LPCVOID)(uintptr_t)addr, &mbi, sizeof mbi)) {
        const uint64_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & writable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            buf.resize(size);
            SIZE_T got = 0;
            if (ReadProcessMemory(p, mbi.BaseAddress, buf.data(), size, &got) || got > 0) {
                for (uint64_t o = 0; o + 4096 <= got; o += 4096)
                    recs.push_back({(uint32_t)(base + o), Hash4K(buf.data() + o), mbi.Protect, mbi.Type});
                bytes += got;
            }
        }
        addr = base + size;
    }
    CloseHandle(p);
    FILE* f = fopen(out, "wb");
    if (!f) return 1;
    uint32_t n = (uint32_t)recs.size();
    fwrite("MKSC", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(recs.data(), sizeof(Rec), n, f);
    fclose(f);
    printf("scanned %u pages (%.1f MiB) in %lu ms\n", n, bytes / 1048576.0, GetTickCount() - t0);
    return 0;
}

std::unordered_map<uint32_t, Rec> Load(const char* path) {
    std::unordered_map<uint32_t, Rec> m;
    FILE* f = fopen(path, "rb");
    if (!f) return m;
    char magic[4]; uint32_t n = 0;
    fread(magic, 1, 4, f); fread(&n, 4, 1, f);
    std::vector<Rec> v(n);
    fread(v.data(), sizeof(Rec), n, f);
    fclose(f);
    for (auto& r : v) m[r.page] = r;
    return m;
}

int Diff(const char* na, const char* nb, const char* sa, const char* sb) {
    auto NA = Load(na), NB = Load(nb), SA = Load(sa), SB = Load(sb);
    auto changed = [](auto& a, auto& b) {
        std::vector<uint32_t> out;
        for (auto& [pg, r] : b) { auto it = a.find(pg); if (it == a.end() || it->second.hash != r.hash) out.push_back(pg); }
        std::sort(out.begin(), out.end());
        return out;
    };
    // Files: NA, NB = two paused scans; SA = scan after the step (taken after NB); SB = later paused scan.
    // noise = changed(NA,NB) U changed(SA,SB); step = changed(NB,SA) minus noise.
    auto n1 = changed(NA, NB), n2 = changed(SA, SB), step = changed(NB, SA);
    std::unordered_map<uint32_t, bool> isNoise;
    for (auto pg : n1) isNoise[pg] = true;
    for (auto pg : n2) isNoise[pg] = true;
    std::vector<uint32_t> noise;
    for (auto& kv : isNoise) noise.push_back(kv.first);
    std::vector<uint32_t> sim;
    for (auto pg : step) if (!isNoise.count(pg)) sim.push_back(pg);
    printf("noise pages changed: %zu ; step pages changed: %zu ; step-only (simulation candidates): %zu (%.1f KiB)\n",
           noise.size(), step.size(), sim.size(), sim.size() * 4.0);
    // Coalesce into runs and print with module attribution.
    for (size_t i = 0; i < sim.size();) {
        size_t j = i;
        while (j + 1 < sim.size() && sim[j + 1] == sim[j] + 4096) ++j;
        const Rec& r = SA[sim[i]];
        printf("  %08X-%08X  %3zu pages  prot=%03lX type=%s\n", sim[i], sim[j] + 4096, j - i + 1, (unsigned long)r.protect,
               r.type == MEM_IMAGE ? "image" : r.type == MEM_MAPPED ? "mapped" : "private");
        i = j + 1;
    }
    return 0;
}
// Files <prefix>-<k>-{n1,n2,s,n3}.bin for k = 1..K. Noise = union over every paused pair;
// a page is a simulation page if it changed across the step in ALL K trials and never in noise.
int MultiDiff(const char* prefix, int K) {
    std::unordered_map<uint32_t, int> stepCount;
    std::unordered_map<uint32_t, bool> noise;
    std::unordered_map<uint32_t, Rec> last;
    auto changed = [](auto& a, auto& b, auto fn) {
        for (auto& [pg, r] : b) { auto it = a.find(pg); if (it == a.end() || it->second.hash != r.hash) fn(pg); }
    };
    for (int k = 1; k <= K; ++k) {
        char p[4][512];
        const char* sfx[4] = {"n1", "n2", "s", "n3"};
        for (int i = 0; i < 4; ++i) snprintf(p[i], sizeof p[i], "%s-%d-%s.bin", prefix, k, sfx[i]);
        auto N1 = Load(p[0]), N2 = Load(p[1]), S = Load(p[2]), N3 = Load(p[3]);
        changed(N1, N2, [&](uint32_t pg) { noise[pg] = true; });
        changed(S, N3, [&](uint32_t pg) { noise[pg] = true; });
        changed(N2, S, [&](uint32_t pg) { stepCount[pg]++; });
        if (k == K) last = S;
    }
    std::vector<uint32_t> all, some;
    for (auto& [pg, c] : stepCount) {
        if (noise.count(pg)) continue;
        (c == K ? all : some).push_back(pg);
    }
    std::sort(all.begin(), all.end());
    printf("trials=%d noise pages=%zu ; step pages in ALL trials (no noise)=%zu (%.1f KiB) ; in some trials=%zu\n", K,
           noise.size(), all.size(), all.size() * 4.0, some.size());
    std::map<std::string, size_t> byType;
    for (size_t i = 0; i < all.size();) {
        size_t j = i;
        while (j + 1 < all.size() && all[j + 1] == all[j] + 4096) ++j;
        const Rec& r = last[all[i]];
        const char* t = r.type == MEM_IMAGE ? "image" : r.type == MEM_MAPPED ? "mapped" : "private";
        byType[t] += j - i + 1;
        printf("  %08X-%08X  %3zu pages  %s\n", all[i], all[j] + 4096, j - i + 1, t);
        i = j + 1;
    }
    for (auto& [t, n] : byType) printf("total %s: %zu pages\n", t.c_str(), n);
    return 0;
}
// BFS over pointer-like dwords starting at the given root objects; every target in
// committed private/mapped memory is visited (reading `span` bytes there).
// Prints, for each page of a write-set listing (multidiff output), the minimum hop
// distance from a root and the root it was reached from.
//   memscan ptrwalk <pid> <multidiff.txt> <maxDepth> <span> <name=hexaddr:size> ...
int PtrWalk(DWORD pid, const char* listing, int maxDepth, uint32_t span, int nroots, char** roots) {
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) return 1;
    auto readable = [&](uint32_t a) {
        MEMORY_BASIC_INFORMATION m;
        if (a < 0x10000 || !VirtualQueryEx(p, (LPCVOID)(uintptr_t)a, &m, sizeof m)) return false;
        return m.State == MEM_COMMIT && m.Type != MEM_IMAGE && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
               (m.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY));
    };
    struct Item { uint32_t addr, size; int depth, root; };
    std::vector<Item> q;
    std::vector<std::string> rootNames;
    std::unordered_map<uint32_t, std::pair<int, int>> pageHit;  // page -> (depth, root)
    std::unordered_map<uint32_t, bool> visited;
    for (int i = 0; i < nroots; ++i) {
        std::string s = roots[i];
        auto eq = s.find('='), colon = s.find(':');
        rootNames.push_back(s.substr(0, eq));
        q.push_back({(uint32_t)strtoul(s.substr(eq + 1, colon - eq - 1).c_str(), nullptr, 16),
                     (uint32_t)strtoul(s.substr(colon + 1).c_str(), nullptr, 16), 0, i});
    }
    std::vector<uint8_t> buf;
    size_t visitedCount = 0;
    for (size_t qi = 0; qi < q.size() && visitedCount < 200000; ++qi) {
        Item it = q[qi];
        if (visited.count(it.addr)) continue;
        visited[it.addr] = true;
        ++visitedCount;
        buf.resize(it.size);
        SIZE_T got = 0;
        if (!ReadProcessMemory(p, (LPCVOID)(uintptr_t)it.addr, buf.data(), it.size, &got) || got < 4) continue;
        for (uint32_t pg = it.addr & ~0xFFFu; pg < it.addr + got; pg += 4096) {
            auto f = pageHit.find(pg);
            if (f == pageHit.end() || f->second.first > it.depth) pageHit[pg] = {it.depth, it.root};
        }
        if (it.depth >= maxDepth) continue;
        for (size_t o = 0; o + 4 <= got; o += 4) {
            uint32_t v;
            memcpy(&v, buf.data() + o, 4);
            if (v < 0x01000000 || v >= 0x7FFF0000 || (v & 3) || visited.count(v)) continue;
            if (!readable(v)) continue;
            q.push_back({v, span, it.depth + 1, it.root});
        }
    }
    CloseHandle(p);
    // Read write-set runs from the multidiff listing.
    FILE* f = fopen(listing, "r");
    if (!f) return 1;
    char line[256];
    size_t total = 0, reached = 0;
    std::map<std::string, size_t> perRoot;
    std::map<int, size_t> perDepth;
    while (fgets(line, sizeof line, f)) {
        unsigned a, b;
        if (sscanf(line, " %x-%x", &a, &b) != 2) continue;
        for (uint32_t pg = a; pg < b; pg += 4096) {
            ++total;
            auto h = pageHit.find(pg);
            if (h == pageHit.end()) continue;
            ++reached;
            perRoot[rootNames[h->second.second]]++;
            perDepth[h->second.first]++;
        }
    }
    fclose(f);
    printf("visited %zu objects; write-set pages %zu, reachable from roots %zu\n", visitedCount, total, reached);
    for (auto& [r, n] : perRoot) printf("  via %s: %zu pages\n", r.c_str(), n);
    for (auto& [d, n] : perDepth) printf("  depth %d: %zu pages\n", d, n);
    return 0;
}
// ptrpages <pid> <out.txt> <maxDepth> <span> <name=addr:size>...
// Writes one line per reached page: "page depth root object parentObject parentOffset".
int PtrPages(DWORD pid, const char* out, int maxDepth, uint32_t span, int nroots, char** roots) {
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) return 1;
    auto readable = [&](uint32_t a) {
        MEMORY_BASIC_INFORMATION m;
        if (a < 0x10000 || !VirtualQueryEx(p, (LPCVOID)(uintptr_t)a, &m, sizeof m)) return false;
        return m.State == MEM_COMMIT && m.Type != MEM_IMAGE && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
               (m.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY));
    };
    struct Item { uint32_t addr, size; int depth, root; uint32_t parent, poff; };
    std::vector<Item> q;
    std::vector<std::string> names;
    for (int i = 0; i < nroots; ++i) {
        std::string s = roots[i];
        auto eq = s.find('='), colon = s.find(':');
        names.push_back(s.substr(0, eq));
        q.push_back({(uint32_t)strtoul(s.substr(eq + 1, colon - eq - 1).c_str(), nullptr, 16),
                     (uint32_t)strtoul(s.substr(colon + 1).c_str(), nullptr, 16), 0, i, 0, 0});
    }
    std::unordered_map<uint32_t, bool> visited;
    std::map<uint32_t, Item> pageOwner;
    std::vector<uint8_t> buf;
    for (size_t qi = 0; qi < q.size() && qi < 300000; ++qi) {
        Item it = q[qi];
        if (visited.count(it.addr)) continue;
        visited[it.addr] = true;
        buf.resize(it.size);
        SIZE_T got = 0;
        if (!ReadProcessMemory(p, (LPCVOID)(uintptr_t)it.addr, buf.data(), it.size, &got) || got < 4) continue;
        for (uint32_t pg = it.addr & ~0xFFFu; pg < it.addr + got; pg += 4096)
            if (!pageOwner.count(pg) || pageOwner[pg].depth > it.depth) pageOwner[pg] = it;
        if (it.depth >= maxDepth) continue;
        for (size_t o = 0; o + 4 <= got; o += 4) {
            uint32_t v;
            memcpy(&v, buf.data() + o, 4);
            if (v < 0x01000000 || v >= 0x7FFF0000 || (v & 3) || visited.count(v) || !readable(v)) continue;
            q.push_back({v, span, it.depth + 1, it.root, it.addr, (uint32_t)o});
        }
    }
    CloseHandle(p);
    FILE* f = fopen(out, "w");
    if (!f) return 1;
    for (auto& [pg, it] : pageOwner)
        fprintf(f, "%08X d=%d root=%s obj=%08X parent=%08X+%X\n", pg, it.depth, names[it.root].c_str(), it.addr,
                it.parent, it.poff);
    fclose(f);
    printf("ptrpages: %zu pages\n", pageOwner.size());
    return 0;
}

// uncovered <multidiff.txt> <spans.txt>: write-set pages not covered by any snapshot span.
int Uncovered(const char* listing, const char* spansFile) {
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    FILE* sf = fopen(spansFile, "r");
    if (!sf) return 1;
    char line[256];
    while (fgets(line, sizeof line, sf)) {
        unsigned b, s;
        if (sscanf(line, "%x %x", &b, &s) == 2) spans.emplace_back(b, b + s);
    }
    fclose(sf);
    FILE* f = fopen(listing, "r");
    if (!f) return 1;
    size_t total = 0, covered = 0, partial = 0;
    std::vector<uint32_t> out;
    while (fgets(line, sizeof line, f)) {
        unsigned a, b;
        if (sscanf(line, " %x-%x", &a, &b) != 2) continue;
        for (uint32_t pg = a; pg < b; pg += 4096) {
            ++total;
            uint32_t cov = 0;
            for (auto& [lo, hi] : spans) {
                const uint32_t s = lo > pg ? lo : pg, e = hi < pg + 4096 ? hi : pg + 4096;
                if (e > s) cov += e - s;
            }
            if (cov >= 4096) ++covered;
            else {
                if (cov) ++partial;
                out.push_back(pg);
            }
        }
    }
    fclose(f);
    printf("write-set pages %zu: fully covered %zu, uncovered %zu (of which partially covered %zu)\n", total, covered,
           out.size(), partial);
    for (size_t i = 0; i < out.size();) {
        size_t j = i;
        while (j + 1 < out.size() && out[j + 1] == out[j] + 4096) ++j;
        printf("  %08X-%08X %zu pages\n", out[i], out[j] + 4096, j - i + 1);
        i = j + 1;
    }
    return 0;
}

// xrefs <pid> <lo> <hi> <out.txt>: every aligned dword in writable committed memory
// OUTSIDE [lo,hi) whose value lies inside [lo,hi). One line per cell: "cell value region-base type".
int XRefs(DWORD pid, uint32_t lo, uint32_t hi, const char* out) {
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) return 1;
    FILE* f = fopen(out, "w");
    if (!f) return 1;
    std::vector<uint8_t> buf;
    uint64_t addr = 0x10000;
    size_t cells = 0;
    std::map<uint32_t, size_t> perAlloc;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x80000000ull && VirtualQueryEx(p, (LPCVOID)(uintptr_t)addr, &mbi, sizeof mbi)) {
        const uint64_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & writable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            buf.resize(size);
            SIZE_T got = 0;
            if (ReadProcessMemory(p, mbi.BaseAddress, buf.data(), size, &got) || got > 0) {
                for (uint64_t o = 0; o + 4 <= got; o += 4) {
                    const uint32_t cell = (uint32_t)(base + o);
                    if (cell >= lo && cell < hi) continue;
                    uint32_t v;
                    memcpy(&v, buf.data() + o, 4);
                    if (v < lo || v >= hi) continue;
                    fprintf(f, "%08X %08X %08X %s\n", cell, v, (uint32_t)(uintptr_t)mbi.AllocationBase,
                            mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private");
                    ++cells;
                    perAlloc[(uint32_t)(uintptr_t)mbi.AllocationBase]++;
                }
            }
        }
        addr = base + size;
    }
    fclose(f);
    CloseHandle(p);
    printf("xrefs into [%08X,%08X): %zu cells\n", lo, hi, cells);
    for (auto& [a, n] : perAlloc) printf("  allocation %08X: %zu cells\n", a, n);
    return 0;
}

// Full raw snapshot of writable pages: "MKFS", u32 count, then {u32 page; u8 data[4096]}.
int SnapFull(DWORD pid, const char* out) {
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) return 1;
    FILE* f = fopen(out, "wb");
    if (!f) return 1;
    uint32_t n = 0;
    fwrite("MKFS", 1, 4, f);
    fwrite(&n, 4, 1, f);
    std::vector<uint8_t> buf;
    uint64_t addr = 0x10000;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD t0 = GetTickCount();
    while (addr < 0x80000000ull && VirtualQueryEx(p, (LPCVOID)(uintptr_t)addr, &mbi, sizeof mbi)) {
        const uint64_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & writable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            buf.resize(size);
            SIZE_T got = 0;
            if (ReadProcessMemory(p, mbi.BaseAddress, buf.data(), size, &got) || got > 0) {
                for (uint64_t o = 0; o + 4096 <= got; o += 4096) {
                    uint32_t pg = (uint32_t)(base + o);
                    fwrite(&pg, 4, 1, f);
                    fwrite(buf.data() + o, 1, 4096, f);
                    ++n;
                }
            }
        }
        addr = base + size;
    }
    fseek(f, 4, SEEK_SET);
    fwrite(&n, 4, 1, f);
    fclose(f);
    CloseHandle(p);
    printf("snapfull %u pages in %lu ms\n", n, GetTickCount() - t0);
    return 0;
}

std::unordered_map<uint32_t, std::vector<uint8_t>> LoadFull(const char* path, const std::unordered_map<uint32_t, bool>* only) {
    std::unordered_map<uint32_t, std::vector<uint8_t>> m;
    FILE* f = fopen(path, "rb");
    if (!f) return m;
    char magic[4];
    uint32_t n = 0;
    fread(magic, 1, 4, f);
    fread(&n, 4, 1, f);
    std::vector<uint8_t> page(4096);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t pg;
        if (fread(&pg, 4, 1, f) != 1 || fread(page.data(), 1, 4096, f) != 4096) break;
        if (!only || only->count(pg)) m[pg] = page;
    }
    fclose(f);
    return m;
}

// fulldiff <A.full> <B.full> <noise1.bin> <noise2.bin> <pages.txt> <max>
//  pages.txt: lines "page depth root parent offset" from ptrwalk --emit; only those pages are compared.
int FullDiff(const char* a, const char* b, const char* n1, const char* n2, const char* pagesFile, int maxRows) {
    std::unordered_map<uint32_t, bool> want;
    std::unordered_map<uint32_t, std::string> info;
    FILE* pf = fopen(pagesFile, "r");
    if (!pf) return 1;
    char line[256];
    while (fgets(line, sizeof line, pf)) {
        unsigned pg;
        if (sscanf(line, "%x", &pg) == 1) {
            want[pg] = true;
            line[strcspn(line, "\r\n")] = 0;
            info[pg] = line;
        }
    }
    fclose(pf);
    auto N1 = Load(n1), N2 = Load(n2);
    for (auto& [pg, r] : N2) { auto it = N1.find(pg); if (it == N1.end() || it->second.hash != r.hash) want.erase(pg); }
    auto A = LoadFull(a, &want), B = LoadFull(b, &want);
    std::vector<uint32_t> pages;
    for (auto& kv : A) if (B.count(kv.first)) pages.push_back(kv.first);
    std::sort(pages.begin(), pages.end());
    int rows = 0;
    size_t diffPages = 0, diffDwords = 0;
    for (uint32_t pg : pages) {
        const auto& x = A[pg];
        const auto& y = B[pg];
        if (!memcmp(x.data(), y.data(), 4096)) continue;
        ++diffPages;
        printf("page %s\n", info[pg].c_str());
        for (int o = 0; o < 4096; o += 4) {
            uint32_t u, v;
            memcpy(&u, x.data() + o, 4);
            memcpy(&v, y.data() + o, 4);
            if (u == v) continue;
            ++diffDwords;
            float fu, fv;
            memcpy(&fu, &u, 4);
            memcpy(&fv, &v, 4);
            if (rows++ < maxRows) printf("   %08X: %08X -> %08X   (f %g -> %g)\n", pg + o, u, v, fu, fv);
        }
    }
    printf("compared %zu pages (fighter graph, noise-free); differing pages %zu, dwords %zu\n", pages.size(), diffPages,
           diffDwords);
    return 0;
}
// rbdiff <A.full> <B.full> <noise1.bin> <noise2.bin> <ownranges.txt> <addrs_out.txt> <maxRows>
// Streams two snapfull files (both in ascending page order), skips pages that changed between
// the two paused hash scans (noise) and pages overlapping the DLL's own snapshot buffers, and
// reports differing dwords. One address per changed 64-byte chunk goes to addrs_out.txt.
int RbDiff(const char* a, const char* b, const char* n1, const char* n2, const char* own, const char* addrsOut, int maxRows) {
    auto N1 = Load(n1), N2 = Load(n2);
    std::unordered_map<uint32_t, bool> noise;
    for (auto& [pg, r] : N2) { auto it = N1.find(pg); if (it == N1.end() || it->second.hash != r.hash) noise[pg] = true; }
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (FILE* of = fopen(own, "r")) {
        unsigned s, e;
        while (fscanf(of, "%x %x", &s, &e) == 2) ranges.push_back({s, e});
        fclose(of);
    }
    auto owned = [&](uint32_t pg) {
        for (auto& r : ranges) if (pg + 4096 > r.first && pg < r.second) return true;
        return false;
    };
    FILE* fa = fopen(a, "rb");
    FILE* fb = fopen(b, "rb");
    FILE* fo = fopen(addrsOut, "w");
    if (!fa || !fb || !fo) return 1;
    char magic[4];
    uint32_t na = 0, nb = 0;
    fread(magic, 1, 4, fa); fread(&na, 4, 1, fa);
    fread(magic, 1, 4, fb); fread(&nb, 4, 1, fb);
    std::vector<uint8_t> pa(4096), pb(4096);
    uint32_t pga = 0, pgb = 0, ia = 0, ib = 0;
    auto nextA = [&]() { return ia++ < na && fread(&pga, 4, 1, fa) == 1 && fread(pa.data(), 1, 4096, fa) == 4096; };
    auto nextB = [&]() { return ib++ < nb && fread(&pgb, 4, 1, fb) == 1 && fread(pb.data(), 1, 4096, fb) == 4096; };
    bool ha = nextA(), hb = nextB();
    size_t pages = 0, diffPages = 0, diffDwords = 0, skippedNoise = 0, skippedOwn = 0;
    int rows = 0;
    while (ha && hb) {
        if (pga < pgb) { ha = nextA(); continue; }
        if (pgb < pga) { hb = nextB(); continue; }
        ++pages;
        if (memcmp(pa.data(), pb.data(), 4096)) {
            if (noise.count(pga)) ++skippedNoise;
            else if (owned(pga)) ++skippedOwn;
            else {
                ++diffPages;
                int lastChunk = -1, pageRows = 0;
                for (int o = 0; o < 4096; o += 4) {
                    uint32_t u, v;
                    memcpy(&u, pa.data() + o, 4);
                    memcpy(&v, pb.data() + o, 4);
                    if (u == v) continue;
                    ++diffDwords;
                    if (o / 64 != lastChunk) {
                        lastChunk = o / 64;
                        fprintf(fo, "%08X\n", pga + o);
                    }
                    float fu, fv;
                    memcpy(&fu, &u, 4);
                    memcpy(&fv, &v, 4);
                    if (rows < maxRows && pageRows++ < 6) {
                        printf("   %08X: %08X -> %08X   (f %g -> %g)\n", pga + o, u, v, fu, fv);
                        ++rows;
                    }
                }
            }
        }
        ha = nextA();
        hb = nextB();
    }
    fclose(fa); fclose(fb); fclose(fo);
    printf("compared %zu pages; differing %zu pages / %zu dwords; skipped noise %zu, own buffers %zu\n", pages, diffPages,
           diffDwords, skippedNoise, skippedOwn);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 9 && !strcmp(argv[1], "rbdiff")) return RbDiff(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], atoi(argv[8]));
    if (argc == 4 && !strcmp(argv[1], "uncovered")) return Uncovered(argv[2], argv[3]);
    if (argc == 6 && !strcmp(argv[1], "xrefs"))
        return XRefs((DWORD)atoi(argv[2]), (uint32_t)strtoul(argv[3], nullptr, 16), (uint32_t)strtoul(argv[4], nullptr, 16), argv[5]);
    if (argc >= 7 && !strcmp(argv[1], "ptrpages"))
        return PtrPages((DWORD)atoi(argv[2]), argv[3], atoi(argv[4]), (uint32_t)strtoul(argv[5], nullptr, 16), argc - 6, argv + 6);
    if (argc == 4 && !strcmp(argv[1], "snapfull")) return SnapFull((DWORD)atoi(argv[2]), argv[3]);
    if (argc == 8 && !strcmp(argv[1], "fulldiff")) return FullDiff(argv[2], argv[3], argv[4], argv[5], argv[6], atoi(argv[7]));
    if (argc >= 7 && !strcmp(argv[1], "ptrwalk"))
        return PtrWalk((DWORD)atoi(argv[2]), argv[3], atoi(argv[4]), (uint32_t)strtoul(argv[5], nullptr, 16), argc - 6, argv + 6);
    if (argc == 4 && !strcmp(argv[1], "multidiff")) return MultiDiff(argv[2], atoi(argv[3]));
    if (argc == 4 && !strcmp(argv[1], "scan")) return Scan((DWORD)atoi(argv[2]), argv[3]);
    if (argc == 6 && !strcmp(argv[1], "diff")) return Diff(argv[2], argv[3], argv[4], argv[5]);
    fprintf(stderr, "usage: memscan scan <pid> <out> | diff <noiseA> <noiseB> <stepA> <stepB>\n");
    return 2;
}
