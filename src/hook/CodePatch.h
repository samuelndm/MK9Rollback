#pragma once
#include <cstdint>

// Writes over game code with every other thread of the process suspended, and
// only when no suspended thread's EIP lies inside the bytes being replaced.
// Original bytes are verified before writing and kept for Restore().
namespace CodePatch {

// Returns false (and writes nothing) if the current bytes != expected.
bool Write(uintptr_t address, const uint8_t* expected, const uint8_t* replacement, uint32_t length, const char* name);
// Restores every patch made by Write (reverse order), same safety rules.
bool RestoreAll();
// Restores the one patch made at address (false if there is none or the write failed); other patches stay.
bool Restore(uintptr_t address);

// Serializes the code that suspends the other threads of the process in a player's game (Write/Restore, FpuGuard's
// pinning, the watchdog's dumps): two such threads suspending each other at the same moment would both stay suspended.
// The lab-only debug-register arming (CrashLog WatchWrite/TraceArm/ChangeWatchArm, 'fpu') does not take it yet.
void LockSuspenders();
bool TryLockSuspenders();
void UnlockSuspenders();

// Executable memory for trampolines (never freed while the module is loaded).
void* AllocExec(uint32_t size);

}  // namespace CodePatch
