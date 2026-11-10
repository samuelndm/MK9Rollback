#include "EnvFingerprint.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include "../debug/Logger.h"
#include "HelloProto.h"
#include "../rollback/FpuGuard.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/ScriptRng.h"
#include "../rollback/CrowdRng.h"
#include "../rollback/Snapshot.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace EnvFingerprint {
namespace {
constexpr uint32_t kFnvBasis = 0x811C9DC5u, kFnvPrime = 0x01000193u;
uint32_t Fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * kFnvPrime;
    return h;
}

void ListFiles(const std::wstring& root, const std::wstring& rel, std::vector<std::pair<std::wstring, uint64_t>>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring r = rel + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ListFiles(root, r, out);
        } else {
            std::wstring lower = r;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            out.push_back({lower, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

uint32_t ContentPrint(int* files) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::vector<std::pair<std::wstring, uint64_t>> list;
    ListFiles(dir + L"\\Asset", L"", list);
    std::sort(list.begin(), list.end());
    uint32_t h = kFnvBasis;
    for (const auto& f : list) {
        h = Fnv(h, f.first.data(), f.first.size() * sizeof(wchar_t));
        h = Fnv(h, &f.second, sizeof f.second);
    }
    *files = (int)list.size();
    return list.empty() ? 0 : h;
}

uint32_t DllPrint() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    if (!n) return 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint32_t h = kFnvBasis;
    static uint8_t buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) h = Fnv(h, buf, got);
    CloseHandle(f);
    return h;
}

uint32_t SwitchPrint() {
    using namespace Snapshot;
    const bool flags[] = {gReflectData, gPinFloats,   gListRescue,   gBindState,     gBindStateLoad, gPadFullLoad,  gFxActors,
                          gFxActorsLoad, gMoveReqs,   gMoveReqLoad,  gCostDmg,       gCostDmgLoad,   gMkActors,     gMkActorsLoad,
                          gListRebase,  gScriptData,  gTierEOn,      gScriptScratch, gNameMaps,      gCompList,     gSkelBound,
                          gSkelBoundLoad, gCompWeights, gCompWeightsLoad, gSkelBind, gBoneRotation,  gForeignVtables, gStrictVtable,
                          gFloatFields, gPawnComps,   gTierECamera,  gTierENodes,    gCompSpan,      gSkelArrays,  gCompFloats,
                          gPropSkel,    gPropSkelLoad, gCompLiveData, gMkHelperRevive, gMkBases, gBindPins};
    uint32_t h = kFnvBasis;
    for (bool b : flags) h = Fnv(h, &b, 1);
    // Lockstep vs rollback too: a Safe-mode game and a Rollback game in one session wait on different things, and the
    // launchers' modes were never compared (2026-10-31).
    // Crowd randomness on its own stream or on RNG A (CrowdRng, 2026-11-04): a mixed pair desyncs on crowd stages.
    const uint32_t fp[5] = {FpuGuard::kX87, FpuGuard::kMxcsrControl, Rollback::NetplayGf(), Rollback::Lockstep() ? 1u : 0u,
                            CrowdRng::Active() ? 1u : 0u};
    return Fnv(h, fp, sizeof fp);
}

// <root>\build\clang\MK9Rollback.dll -> <root>\version.txt (the packaged layout, tools\package.ps1).
Stamp ReadStamp() {
    Stamp s{};
    wchar_t path[MAX_PATH + 16];
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    if (!n || n >= MAX_PATH) return s;
    for (int up = 0; up < 3; ++up) {  // the file name and two folders
        wchar_t* cut = nullptr;
        for (wchar_t* c = path; *c; ++c)
            if (*c == L'\\' || *c == L'/') cut = c;
        if (!cut) return s;
        *cut = 0;
    }
    wcscat(path, L"\\version.txt");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    char text[512];
    DWORD got = 0;
    if (!ReadFile(f, text, sizeof text, &got, nullptr)) got = 0;
    CloseHandle(f);
    HelloProto::ParseStamp(text, got, &s.date, &s.time);
    return s;
}

// The cached part: computed once, by whichever of Prepare()'s thread and Get() comes first; the other waits for it.
INIT_ONCE gOnce = INIT_ONCE_STATIC_INIT;
Print gPrint{};
Stamp gStamp{};
volatile LONG gReady = 0;
volatile LONG gThread = 0;  // Prepare(): 0 not called, 1 its thread runs, 2 done

BOOL CALLBACK ComputeOnce(PINIT_ONCE, PVOID where, PVOID*) {
    const ULONGLONG t0 = GetTickCount64();
    int files = 0;
    gPrint.v[kContent] = ContentPrint(&files);
    gPrint.v[kDll] = DllPrint();
    gStamp = ReadStamp();
    char stamp[24];
    Logger::Info("ENVPRINT content %08X (%d files under Asset), dll %08X, release %s (%llu ms on %s)", gPrint.v[kContent], files,
                 gPrint.v[kDll], HelloProto::FormatStamp(gStamp.date, gStamp.time, stamp, sizeof stamp),
                 (unsigned long long)(GetTickCount64() - t0), static_cast<const char*>(where));
    InterlockedExchange(&gReady, 1);
    return TRUE;
}

void Compute(const char* where) { InitOnceExecuteOnce(&gOnce, ComputeOnce, const_cast<char*>(where), nullptr); }

DWORD WINAPI PrepareThread(void*) {
    Compute("a background thread");
    InterlockedExchange(&gThread, 2);
    return 0;
}
}  // namespace

const Print& Get() {
    Compute("the game thread");
    gPrint.v[kSwitches] = SwitchPrint();
    return gPrint;
}

void Prepare() {
    if (gReady || InterlockedCompareExchange(&gThread, 1, 0) != 0) return;
    if (HANDLE t = CreateThread(nullptr, 0, PrepareThread, nullptr, 0, nullptr)) CloseHandle(t);
    else InterlockedExchange(&gThread, 2);  // Get() computes it
}

bool Ready() { return gReady != 0; }
bool Busy() { return gThread == 1; }
uint32_t Switches() { return SwitchPrint(); }

Stamp ReleaseStamp() {
    Compute("the game thread");
    return gStamp;
}

const char* FieldName(int f) {
    static const char* const names[kFields] = {"game content (Asset files)", "MK9Rollback DLL build", "snapshot/FP switches"};
    return f >= 0 && f < kFields ? names[f] : "?";
}

}  // namespace EnvFingerprint
