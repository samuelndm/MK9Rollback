// MK9RollbackInjector (x86).
//   MK9RollbackInjector inject [path\to\MK9Rollback.dll] [pid]
//   MK9RollbackInjector unload
//   MK9RollbackInjector status
// Attaches to a running MKKE.exe, verifies its on-disk SHA-256 matches the
// pinned build, then LoadLibraryW's the DLL (or calls its MK9R_Unload export).
// Nothing is written to the game directory.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "../src/common/Sha256.h"

namespace {
constexpr const char* kExpectedSha256 = "D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6";
constexpr const wchar_t* kDllName = L"MK9Rollback.dll";

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

bool VerifyTarget(HANDLE process) {
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, path, &n)) {
        fprintf(stderr, "QueryFullProcessImageName failed (%lu)\n", GetLastError());
        return false;
    }
    BOOL wow64 = FALSE;
    IsWow64Process(process, &wow64);
    const std::string sha = Sha256File(path);
    printf("Target: %ls\nSHA256: %s\nWOW64 (32-bit): %s\n", path, sha.c_str(), wow64 ? "yes" : "no");
    if (sha != kExpectedSha256) {
        fprintf(stderr, "Unknown MKKE.exe build (expected %s). Refusing.\n", kExpectedSha256);
        return false;
    }
    return true;
}

bool RunRemote(HANDLE process, LPTHREAD_START_ROUTINE fn, void* arg, DWORD* exitCode) {
    HANDLE t = CreateRemoteThread(process, nullptr, 0, fn, arg, 0, nullptr);
    if (!t) {
        fprintf(stderr, "CreateRemoteThread failed (%lu)\n", GetLastError());
        return false;
    }
    WaitForSingleObject(t, 15000);
    GetExitCodeThread(t, exitCode);
    CloseHandle(t);
    return true;
}

int Inject(DWORD pid, const std::wstring& dllPath) {
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "DLL not found: %ls\n", dllPath.c_str());
        return 1;
    }
    if (FindRemoteModule(pid, kDllName)) {
        fprintf(stderr, "MK9Rollback.dll is already loaded; run 'unload' first.\n");
        return 1;
    }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        fprintf(stderr, "OpenProcess failed (%lu)\n", GetLastError());
        return 1;
    }
    int rc = 1;
    if (VerifyTarget(process)) {
        const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD code = 0;
        if (remote && WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr) &&
            RunRemote(process,
                      reinterpret_cast<LPTHREAD_START_ROUTINE>(
                          GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")),
                      remote, &code)) {
            printf(code ? "Injected: module handle 0x%08lX\n" : "LoadLibraryW returned NULL\n", code);
            rc = code ? 0 : 1;
        }
        if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
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
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        fprintf(stderr, "OpenProcess failed (%lu)\n", GetLastError());
        return 1;
    }
    DWORD code = 0;
    const bool ok = RunRemote(process, reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + rva), nullptr, &code);
    CloseHandle(process);
    Sleep(200);
    const bool gone = FindRemoteModule(pid, kDllName) == 0;
    printf("Unload %s; module %s\n", ok ? "requested" : "failed", gone ? "unloaded" : "STILL LOADED");
    return ok && gone ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (sizeof(void*) != 4) {
        fprintf(stderr, "Build this injector as x86.\n");
        return 1;
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

    // Optional third argument: the PID of one MKKE.exe (two-instance tests); its image is still hash-verified.
    const DWORD pid = argc > 3 ? (DWORD)wcstoul(argv[3], nullptr, 10) : FindProcess(L"MKKE.exe");
    if (!pid) {
        fprintf(stderr, "MKKE.exe is not running.\n");
        return 1;
    }
    printf("MKKE.exe pid %lu\n", pid);

    if (cmd == L"inject") return Inject(pid, dll);
    if (cmd == L"unload") return Unload(pid, dll);
    const uintptr_t base = FindRemoteModule(pid, kDllName);
    printf("MK9Rollback.dll: %s (0x%08X)\n", base ? "loaded" : "not loaded", (unsigned)base);
    return 0;
}
