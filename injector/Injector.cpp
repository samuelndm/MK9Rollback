// MK9RollbackInjector (x86).
//   MK9RollbackInjector inject [path\to\MK9Rollback.dll] [pid]
//   MK9RollbackInjector unload
//   MK9RollbackInjector status
// Attaches to a running MKKE.exe, verifies its on-disk SHA-256 matches the
// pinned build, then LoadLibraryW's the DLL (or calls its MK9R_Unload export).
// Nothing is written to the game directory.
// Exit codes (the Play Online window, tools\launcher_lib.ps1 Get-InjectorResult, maps them to player messages):
//   0 done, 1 other failure (game not running, ...), 2 unknown MKKE.exe build, 3 MK9Rollback.dll missing,
//   4 OpenProcess access denied (the game runs elevated), 5 LoadLibraryW returned NULL, 6 already loaded,
//   7 the remote thread did not finish within 15 s.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "../src/common/Sha256.h"

namespace {
constexpr const char* kExpectedSha256 = "D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6";
constexpr const wchar_t* kDllName = L"MK9Rollback.dll";
enum ExitCode : int {
    kExitOk = 0,
    kExitFailed = 1,
    kExitUnknownBuild = 2,
    kExitDllMissing = 3,
    kExitAccessDenied = 4,
    kExitLoadFailed = 5,
    kExitAlreadyLoaded = 6,
    kExitTimeout = 7,
};
enum class RemoteResult { kFailed, kDone, kTimeout };

DWORD FindProcess(const wchar_t* exe) {
    DWORD pid = 0, count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe) == 0) {
            pid = pe.th32ProcessID;
            ++count;
        }
    }
    CloseHandle(snap);
    if (count > 1) {
        fprintf(stderr, "More than one MKKE.exe is running; refusing.\n");
        return 0;
    }
    return pid;
}

uintptr_t FindRemoteModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{sizeof me};
    uintptr_t base = 0;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsicmp(me.szModule, name) == 0) {
            base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            break;
        }
    }
    CloseHandle(snap);
    return base;
}

// kExitOk, or why the target is refused (kExitUnknownBuild / kExitFailed).
int VerifyTarget(HANDLE process) {
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, path, &n)) {
        fprintf(stderr, "QueryFullProcessImageName failed (%lu)\n", GetLastError());
        return kExitFailed;
    }
    BOOL wow64 = FALSE;
    IsWow64Process(process, &wow64);
    const std::string sha = Sha256File(path);
    printf("Target: %ls\nSHA256: %s\nWOW64 (32-bit): %s\n", path, sha.c_str(), wow64 ? "yes" : "no");
    if (sha != kExpectedSha256) {
        fprintf(stderr, "Unknown MKKE.exe build (expected %s). Refusing.\n", kExpectedSha256);
        return kExitUnknownBuild;
    }
    return kExitOk;
}

// kDone only when the thread finished within 15 s; then *exitCode is its return value. On kTimeout the thread may
// still be running and reading its argument: the caller must not free that memory.
RemoteResult RunRemote(HANDLE process, LPTHREAD_START_ROUTINE fn, void* arg, DWORD* exitCode) {
    HANDLE t = CreateRemoteThread(process, nullptr, 0, fn, arg, 0, nullptr);
    if (!t) {
        fprintf(stderr, "CreateRemoteThread failed (%lu)\n", GetLastError());
        return RemoteResult::kFailed;
    }
    const DWORD wait = WaitForSingleObject(t, 15000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "The remote thread did not finish within 15 s (wait result %lu)\n", wait);
        CloseHandle(t);
        return RemoteResult::kTimeout;
    }
    const BOOL got = GetExitCodeThread(t, exitCode);
    CloseHandle(t);
    if (!got) {
        fprintf(stderr, "GetExitCodeThread failed (%lu)\n", GetLastError());
        return RemoteResult::kFailed;
    }
    return RemoteResult::kDone;
}

HANDLE OpenTarget(DWORD pid, int* rc) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        const DWORD err = GetLastError();
        fprintf(stderr, "OpenProcess failed (%lu)%s\n", err,
                err == ERROR_ACCESS_DENIED ? ": access denied (is the game running as administrator?)" : "");
        *rc = err == ERROR_ACCESS_DENIED ? kExitAccessDenied : kExitFailed;
    }
    return process;
}

int Inject(DWORD pid, const std::wstring& dllPath) {
    if (FindRemoteModule(pid, kDllName)) {
        fprintf(stderr, "MK9Rollback.dll is already loaded; run 'unload' first.\n");
        return kExitAlreadyLoaded;
    }
    int rc = kExitFailed;
    HANDLE process = OpenTarget(pid, &rc);
    if (!process) return rc;
    rc = VerifyTarget(process);
    if (rc == kExitOk) {
        rc = kExitFailed;
        const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD code = 0;
        RemoteResult run = RemoteResult::kFailed;
        if (!remote) {
            fprintf(stderr, "VirtualAllocEx failed (%lu)\n", GetLastError());
        } else if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
            fprintf(stderr, "WriteProcessMemory failed (%lu)\n", GetLastError());
        } else {
            run = RunRemote(process,
                            reinterpret_cast<LPTHREAD_START_ROUTINE>(
                                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")),
                            remote, &code);
        }
        if (run == RemoteResult::kDone) {
            if (code) {
                printf("Injected: module handle 0x%08lX\n", code);
                rc = kExitOk;
            } else {
                fprintf(stderr, "LoadLibraryW returned NULL\n");
                rc = kExitLoadFailed;
            }
        } else if (run == RemoteResult::kTimeout) {
            rc = kExitTimeout;
        }
        // A thread still running LoadLibraryW reads the path from this memory: it is left allocated (a few bytes).
        if (remote && run != RemoteResult::kTimeout) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    CloseHandle(process);
    return rc;
}

int Unload(DWORD pid, const std::wstring& dllPath) {
    const uintptr_t remoteBase = FindRemoteModule(pid, kDllName);
    if (!remoteBase) {
        fprintf(stderr, "MK9Rollback.dll is not loaded in the target.\n");
        return 1;
    }
    // Resolve the export's RVA from our local copy without running its DllMain.
    HMODULE local = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) {
        fprintf(stderr, "Cannot map local DLL (%lu)\n", GetLastError());
        return 1;
    }
    FARPROC fn = GetProcAddress(local, "MK9R_Unload");
    const uintptr_t rva = fn ? reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(local) : 0;
    FreeLibrary(local);
    if (!rva) {
        fprintf(stderr, "MK9R_Unload export not found.\n");
        return 1;
    }
    int rc = kExitFailed;
    HANDLE process = OpenTarget(pid, &rc);
    if (!process) return rc;
    DWORD code = 0;
    const RemoteResult run = RunRemote(process, reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + rva), nullptr, &code);
    CloseHandle(process);
    if (run == RemoteResult::kTimeout) return kExitTimeout;
    const bool ok = run == RemoteResult::kDone;
    Sleep(200);
    const bool gone = FindRemoteModule(pid, kDllName) == 0;
    printf("Unload %s; module %s\n", ok ? "requested" : "failed", gone ? "unloaded" : "STILL LOADED");
    return ok && gone ? kExitOk : kExitFailed;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (sizeof(void*) != 4) {
        fprintf(stderr, "Build this injector as x86.\n");
        return kExitFailed;
    }
    const std::wstring cmd = argc > 1 ? argv[1] : L"status";

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dll = self;
    dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + kDllName;
    if (argc > 2) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(argv[2], MAX_PATH, full, nullptr);
        dll = full;
    }
    // Checked before looking for the game: a DLL removed by an antivirus is reported as such even without the game.
    if (cmd == L"inject" && GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "DLL not found: %ls\n", dll.c_str());
        return kExitDllMissing;
    }

    // Optional third argument: the PID of one MKKE.exe (two-instance tests); its image is still hash-verified.
    const DWORD pid = argc > 3 ? (DWORD)wcstoul(argv[3], nullptr, 10) : FindProcess(L"MKKE.exe");
    if (!pid) {
        fprintf(stderr, "MKKE.exe is not running.\n");
        return kExitFailed;
    }
    printf("MKKE.exe pid %lu\n", pid);

    if (cmd == L"inject") return Inject(pid, dll);
    if (cmd == L"unload") return Unload(pid, dll);
    const uintptr_t base = FindRemoteModule(pid, kDllName);
    printf("MK9Rollback.dll: %s (0x%08X)\n", base ? "loaded" : "not loaded", (unsigned)base);
    return 0;
}
