#pragma once
#include <cstdint>

// Diagnostic: attributes large reservations (>= 256 KB, the 1 MB regions that grow during GGPO sessions)
// to their first non-system callers via inline hooks on ntdll's virtual-memory syscall stubs.
namespace AllocWatch {
void Install();
void Uninstall();
void Report();
void SetFrame(uint32_t gf);  // game thread, every save: stamps reservations with the frame
}  // namespace AllocWatch
