#pragma once
#include <cstdint>

// Minimal, reversible code/data patches. Every patch records its original
// bytes and is undone by RemoveAll(). Only aligned 4-byte writes are used, so
// each patch is a single atomic store while the game thread is running.
namespace HookManager {

// Retarget an existing `E8 rel32` CALL at callSite. Fails unless the byte at
// callSite is E8, the current target equals expectedTarget, and the rel32 is
// 4-byte aligned.
bool PatchCall(uintptr_t callSite, uintptr_t expectedTarget, void* newTarget, const char* name);

// Replace a 4-byte pointer slot (e.g. a vtable entry). Fails unless the slot
// currently holds expectedValue.
bool PatchPointer(uintptr_t slot, uintptr_t expectedValue, void* newValue, const char* name);

// Restore every patch in reverse order. Returns false if any restore failed
// (e.g. bytes were changed by someone else).
bool RemoveAll();

}  // namespace HookManager
