#include "AllocWatch.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "CrashLog.h"
#include "Logger.h"

// Inline hooks on ntdll!NtAllocateVirtualMemory / NtFreeVirtualMemory (WoW64 stubs start with the 5-byte
// "mov eax, imm32"; anything else is refused). Every user-mode reservation goes through them, including
// the NT heap's large blocks and segments. Original bytes are kept and restored by Uninstall. The hooks
// never touch the heap (a fixed open-addressing table) so they cannot deadlock against a heap lock.
namespace AllocWatch {
namespace {
using NtAllocFn = LONG(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
using NtFreeFn = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
NtAllocFn gAllocTramp = nullptr;
NtFreeFn gFreeTramp = nullptr;
uint8_t* gAllocTarget = nullptr;
uint8_t* gFreeTarget = nullptr;
uint64_t gAllocOrig = 0, gFreeOrig = 0;
bool gInstalled = false;
constexpr SIZE_T kMin = 256 * 1024;

constexpr int kDepth = 6;
struct Entry {
    uint32_t base, size, gf;
    uint32_t c[kDepth];
    bool game;
};
volatile uint32_t gFrameNo = 0;
volatile DWORD gGameTid = 0;
constexpr uint32_t kCap = 1u << 16;
constexpr uint32_t kTomb = 1;
Entry gTable[kCap];
SRWLOCK gLock = SRWLOCK_INIT;
volatile LONG gLiveCount = 0;

uint32_t Slot(uint32_t base) { return (base >> 16) * 2654435761u >> 16 & (kCap - 1); }
void Insert(const Entry& e) {
    for (uint32_t i = Slot(e.base), n = 0; n < kCap; i = (i + 1) & (kCap - 1), ++n)
        if (gTable[i].base <= kTomb) {
            gTable[i] = e;
            ++gLiveCount;
            return;
        }
}
void Erase(uint32_t base) {
    for (uint32_t i = Slot(base), n = 0; n < kCap && gTable[i].base; i = (i + 1) & (kCap - 1), ++n)
        if (gTable[i].base == base) {
            gTable[i].base = gTable[(i + 1) & (kCap - 1)].base ? kTomb : 0;
            for (uint32_t j = i; gTable[j].base == 0;) {  // trailing tombstones become empty
                j = (j - 1) & (kCap - 1);
                if (gTable[j].base != kTomb) break;
                gTable[j].base = 0;
            }
            --gLiveCount;
            return;
        }
}

struct Range {
    uint32_t lo, hi;
};
Range gSys[3];  // ntdll, kernelbase, kernel32
bool IsSystem(uint32_t a) {
    for (auto& r : gSys)
        if (a >= r.lo && a < r.hi) return true;
    return false;
}
Range ModRange(const char* name) {
    HMODULE m = GetModuleHandleA(name);
    if (!m) return {0, 0};
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(reinterpret_cast<uint8_t*>(m) + dos->e_lfanew);
    return {(uint32_t)(uintptr_t)m, (uint32_t)(uintptr_t)m + nt->OptionalHeader.SizeOfImage};
}

// No thread_local here: ntdll allocates during thread start/exit, when the TLS array (fs:[0x2C]) is still null
// (synctest95/98/99 crashed in this hook reading it). The hooks never allocate, so they cannot recurse.
bool Self(HANDLE ph) { return ph == GetCurrentProcess() || GetProcessId(ph) == GetCurrentProcessId(); }

// The first kDepth return addresses outside ntdll/kernelbase/kernel32 along the EBP chain.
void Callers(uint32_t first, uint32_t fp, uint32_t* c) {
    int k = 0;
    for (int i = 0; i < kDepth; ++i) c[i] = 0;
    uint32_t ret = first;
    for (int depth = 0; depth < 32 && k < kDepth; ++depth) {
        if (ret && !IsSystem(ret) && (k == 0 || ret != c[k - 1])) c[k++] = ret;
        uint32_t next = 0;
        if (!CrashLog::SafeRead32(fp, &next) || next <= fp || next - fp > 0x100000) return;
        fp = next;
        if (!CrashLog::SafeRead32(fp + 4, &ret)) return;
    }
}

LONG NTAPI HkAlloc(HANDLE ph, PVOID* base, ULONG_PTR zb, PSIZE_T size, ULONG type, ULONG prot) {
    const bool fresh = base && !*base;  // VirtualAlloc(NULL, n, MEM_COMMIT) reserves implicitly
    const LONG st = gAllocTramp(ph, base, zb, size, type, prot);
    if (st >= 0 && Self(ph) && ((type & MEM_RESERVE) || fresh) && size && *size >= kMin && base && *base) {
        Entry e{(uint32_t)(uintptr_t)*base, (uint32_t)*size, gFrameNo, {}, GetCurrentThreadId() == gGameTid};
        Callers((uint32_t)(uintptr_t)__builtin_return_address(0), (uint32_t)(uintptr_t)__builtin_frame_address(0), e.c);
        AcquireSRWLockExclusive(&gLock);
        Insert(e);
        ReleaseSRWLockExclusive(&gLock);
    }
    return st;
}
using NtAllocExFn = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
NtAllocExFn gAllocExTramp = nullptr;
uint8_t* gAllocExTarget = nullptr;
uint64_t gAllocExOrig = 0;
LONG NTAPI HkAllocEx(HANDLE ph, PVOID* base, PSIZE_T size, ULONG type, ULONG prot, PVOID ext, ULONG next) {
    const bool fresh = base && !*base;
    const LONG st = gAllocExTramp(ph, base, size, type, prot, ext, next);
    if (st >= 0 && Self(ph) && ((type & MEM_RESERVE) || fresh) && size && *size >= kMin && base && *base) {
        Entry e{(uint32_t)(uintptr_t)*base, (uint32_t)*size, gFrameNo, {}, GetCurrentThreadId() == gGameTid};
        Callers((uint32_t)(uintptr_t)__builtin_return_address(0), (uint32_t)(uintptr_t)__builtin_frame_address(0), e.c);
        AcquireSRWLockExclusive(&gLock);
        Insert(e);
        ReleaseSRWLockExclusive(&gLock);
    }
    return st;
}
LONG NTAPI HkFree(HANDLE ph, PVOID* base, PSIZE_T size, ULONG type) {
    const uint32_t b = base ? (uint32_t)(uintptr_t)*base : 0;
    const LONG st = gFreeTramp(ph, base, size, type);
    if (st >= 0 && b && Self(ph) && (type & MEM_RELEASE)) {
        AcquireSRWLockExclusive(&gLock);
        Erase(b & ~0xFFFFu);
        ReleaseSRWLockExclusive(&gLock);
    }
    return st;
}

bool Hook(const char* name, void* hook, uint8_t** target, uint64_t* orig, void** tramp) {
    auto* t = reinterpret_cast<uint8_t*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), name));
    if (!t || t[0] != 0xB8 || ((uintptr_t)t & 7)) {
        Logger::Info("ALLOCWATCH %s: unexpected prologue (%02X) or alignment; refusing", name, t ? t[0] : 0);
        return false;
    }
    auto* tr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tr) return false;
    memcpy(tr, t, 5);  // mov eax, imm32
    tr[5] = 0xE9;
    const int32_t back = (int32_t)((uintptr_t)(t + 5) - (uintptr_t)(tr + 10));
    memcpy(tr + 6, &back, 4);
    uint64_t old;
    memcpy(&old, t, 8);
    uint8_t nb[8];
    memcpy(nb, &old, 8);
    nb[0] = 0xE9;
    const int32_t rel = (int32_t)((uintptr_t)hook - (uintptr_t)(t + 5));
    memcpy(nb + 1, &rel, 4);
    uint64_t nv;
    memcpy(&nv, nb, 8);
    DWORD prot;
    if (!VirtualProtect(t, 8, PAGE_EXECUTE_READWRITE, &prot)) return false;
    *tramp = tr;
    const bool ok = (uint64_t)InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(t), (LONG64)nv, (LONG64)old) == old;
    VirtualProtect(t, 8, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), t, 8);
    if (!ok) return false;
    *target = t;
    *orig = old;
    Logger::Info("ALLOCWATCH hooked ntdll!%s at %p (original %016llX)", name, t, (unsigned long long)old);
    return true;
}
void Unhook(uint8_t* t, uint64_t orig) {
    if (!t) return;
    DWORD prot;
    if (!VirtualProtect(t, 8, PAGE_EXECUTE_READWRITE, &prot)) return;
    uint64_t cur;
    memcpy(&cur, t, 8);
    InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(t), (LONG64)orig, (LONG64)cur);
    VirtualProtect(t, 8, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), t, 8);
}

void Describe(uint32_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>((uintptr_t)a), &m) &&
        GetModuleFileNameA(m, path, sizeof path)) {
        const char* b = strrchr(path, '\\');
        snprintf(out, n, "%s+%X", b ? b + 1 : path, a - (uint32_t)(uintptr_t)m);
    } else {
        snprintf(out, n, "%08X", a);
    }
}
}  // namespace

void Install() {
    if (gInstalled) return;
    gSys[0] = ModRange("ntdll.dll");
    gSys[1] = ModRange("kernelbase.dll");
    gSys[2] = ModRange("kernel32.dll");
    if (!Hook("NtAllocateVirtualMemory", (void*)&HkAlloc, &gAllocTarget, &gAllocOrig, reinterpret_cast<void**>(&gAllocTramp)))
        return;
    if (!Hook("NtFreeVirtualMemory", (void*)&HkFree, &gFreeTarget, &gFreeOrig, reinterpret_cast<void**>(&gFreeTramp))) {
        Unhook(gAllocTarget, gAllocOrig);
        gAllocTarget = nullptr;
        return;
    }
    if (!Hook("NtAllocateVirtualMemoryEx", (void*)&HkAllocEx, &gAllocExTarget, &gAllocExOrig, reinterpret_cast<void**>(&gAllocExTramp)))
        Logger::Info("ALLOCWATCH: NtAllocateVirtualMemoryEx not hooked (continuing)");
    gInstalled = true;
}

void Uninstall() {
    if (!gInstalled) return;
    Unhook(gAllocTarget, gAllocOrig);
    Unhook(gFreeTarget, gFreeOrig);
    Unhook(gAllocExTarget, gAllocExOrig);
    gInstalled = false;  // trampolines stay allocated: a thread may still be inside one
}

void Report() {
    if (!gInstalled) return;
    std::vector<Entry> copy;
    copy.reserve(kCap);  // allocate before taking the lock (the hooks never wait on the heap)
    AcquireSRWLockShared(&gLock);
    for (uint32_t i = 0; i < kCap; ++i)
        if (gTable[i].base > kTomb) copy.push_back(gTable[i]);
    ReleaseSRWLockShared(&gLock);
    struct Agg {
        uint32_t count = 0, game = 0, minGf = 0xFFFFFFFF, maxGf = 0;
        uint64_t bytes = 0;
        const Entry* sample = nullptr;
    };
    std::map<std::vector<uint32_t>, Agg> by;
    for (auto& e : copy) {
        auto& a = by[std::vector<uint32_t>(e.c, e.c + kDepth)];
        ++a.count;
        a.game += e.game;
        a.bytes += e.size;
        a.minGf = std::min(a.minGf, e.gf);
        a.maxGf = std::max(a.maxGf, e.gf);
        a.sample = &e;
    }
    std::vector<const std::pair<const std::vector<uint32_t>, Agg>*> v;
    for (auto& p : by) v.push_back(&p);
    std::sort(v.begin(), v.end(), [](auto* x, auto* y) { return x->second.bytes > y->second.bytes; });
    Logger::Info("ALLOCWATCH %u live reservations >= 256 KB", (unsigned)copy.size());
    for (size_t i = 0; i < v.size() && i < 6; ++i) {
        char chain[700];
        int n = 0;
        for (uint32_t c : v[i]->first) {
            if (!c) break;
            char d[96];
            Describe(c, d, sizeof d);
            n += snprintf(chain + n, sizeof chain - n, " %s", d);
        }
        chain[n] = 0;
        const Agg& a = v[i]->second;
        Logger::Info("ALLOCWATCH   %u live, %llu KB, %u on the game thread, gf %u..%u:%s", a.count,
                     (unsigned long long)(a.bytes >> 10), a.game, a.minGf, a.maxGf, chain);
    }
}

void SetFrame(uint32_t gf) {
    gFrameNo = gf;
    gGameTid = GetCurrentThreadId();
}
}  // namespace AllocWatch