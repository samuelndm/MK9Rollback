// MK9Rollback - Milestone 1/2 foundation.
// Loads into MKKE.exe (via injector), verifies the exact build, installs two
// observation-only hooks and logs simulation frames. No gameplay changes.

#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "debug/CommandChannel.h"
#include "debug/CrashLog.h"
#include "debug/Logger.h"
#include "debug/Overlay.h"
#include "game/MK9Game.h"
#include "hook/CodePatch.h"
#include "hook/GameLoopHook.h"
#include "hook/ObjectHooks.h"
#include "hook/Ue3Refs.h"
#include "net/OnlineMode.h"
#include "rollback/FramePace.h"
#include "rollback/FreeQuarantine.h"
#include "rollback/ParallelMem.h"
#include "rollback/RenderTag.h"
#include "rollback/Snapshot.h"

namespace {
HMODULE gModule = nullptr;
volatile LONG gHooksInstalled = 0;
volatile LONG gUnloading = 0;

void ModuleDirectory(wchar_t* out, DWORD n) {
    GetModuleFileNameW(gModule, out, n);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

DWORD WINAPI InitThread(LPVOID) {
    wchar_t dir[MAX_PATH];
    ModuleDirectory(dir, MAX_PATH);
    // A stale command file goes before the log exists: the launchers write their commands as soon as they see
    // MK9Rollback.log, and removing the file later (CommandChannel::Start, after the build check) deleted them.
    {
        wchar_t cmd[MAX_PATH];
        swprintf(cmd, MAX_PATH, L"%ls\\MK9Rollback_cmd.txt", dir);
        _wremove(cmd);
    }
    Logger::Open(dir);
    Logger::Info("[MK9Rollback] ===== start (pid %lu, module 0x%08X) =====", GetCurrentProcessId(),
                 (uintptr_t)gModule);

    MK9Game::Addresses addr{};
    if (!MK9Game::VerifyBuildAndResolve(&addr)) {
        Logger::Info("Inert: build verification failed.");
        return 0;
    }
    if (InterlockedCompareExchange(&gUnloading, 0, 0)) return 0;
    // Everything the hooks touch must exist before the first hook goes live (run44: the
    // engine-tick hook ran RunFrame on an uninitialized critical section).
    CrashLog::Install();
    CommandChannel::Start(dir);
    // Track UE3 objects that hold raw MK pointers from the start: instances created before
    // tracking are invisible to rollback loads (run43 crash).
    if (!Ue3Refs::Install()) Logger::Info("UE3 reference tracking not installed");
    if (!GameLoopHook::Install()) {
        Logger::Info("Inert: hook installation refused.");
        return 0;
    }
    InterlockedExchange(&gHooksInstalled, 1);
    // A match must keep its pace when the window is not in front (alt-tab, a chat window over it): no EcoQoS / efficiency
    // cores or lowered timer resolution from Windows for this process.
    FramePace::SetPowerThrottling(false);
    // In-game status line (render thread, drawn before MKKE's EndScene); display only, the hooks above do not need it.
    if (!Overlay::Install()) Logger::Info("OVERLAY not installed (the game runs without the status line)");
    Logger::Info("Hook initialized successfully");
    return 0;
}
}  // namespace

// Called by the injector through CreateRemoteThread. Restores every patch,
// waits for in-flight hook calls to drain, then unloads this module.
extern "C" __declspec(dllexport) DWORD WINAPI MK9R_Unload(LPVOID) {
    InterlockedExchange(&gUnloading, 1);
    Overlay::Disable();  // the stub passes through until CodePatch::RestoreAll removes it
    RenderTag::Disable();  // frames queued from here on carry the game's own render command again
    if (InterlockedExchange(&gHooksInstalled, 0)) {
        GameLoopHook::Uninstall();
        for (int i = 0; i < 200 && GameLoopHook::InFlight() != 0; ++i) Sleep(10);
        Sleep(100);  // let any thread that read the old pointer just before restore leave our code
        FreeQuarantine::Uninstall();  // vtable slots already restored by GameLoopHook::Uninstall (HookManager)
        CommandChannel::Stop();
        CommandChannel::Shutdown();  // hooks are gone, so the game thread no longer touches override state
        {  // CodePatch users (object hooks, Ue3Refs, the overlay, ...) are restored together
            ObjectHooks::Uninstall();
            CodePatch::RestoreAll();
            for (int i = 0; i < 200 && (ObjectHooks::InFlight() != 0 || Overlay::InFlight() != 0 || Online::ScanRunning()); ++i) Sleep(10);
            Sleep(100);  // threads that entered a naked stub just before restore leave it
        }
        Logger::Info("Unhooked; in-flight=%ld; simulation frames observed=%llu", GameLoopHook::InFlight(),
                     (unsigned long long)GameLoopHook::gSimulationFrame);
    } else {
        // The game-loop hooks were refused: CrashLog's out-of-memory patch and Ue3Refs went in before, and the command
        // thread runs.
        CommandChannel::Stop();
        CodePatch::RestoreAll();
        Sleep(100);
    }
    Snapshot::StopPageWarmer();
    ParallelMem::Stop();
    CrashLog::StopWatchdog();
    CrashLog::Uninstall();
    Logger::Info("[MK9Rollback] ===== unload =====");
    Logger::Close();
    FreeLibraryAndExitThread(gModule, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        // Process exit: hooks die with the process; say who ended it, persist logs.
        CrashLog::LogProcessExit();
        Logger::Flush();
    }
    return TRUE;
}



