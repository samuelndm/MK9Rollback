#pragma once
#include <cstdint>

// Vectored exception handler that records access violations / illegal instructions
// with the current breadcrumb (what MK9Rollback was doing) before the game dies.
namespace CrashLog {
void Install();
void Uninstall();
// Cheap: stores a pointer to a static string (no allocation).
void Crumb(const char* what);
// GMalloc block lookup 0x00405A10 (out[1] = block size) that cannot crash: an access
// violation inside it resumes at a recovery label and returns false. Game thread only.
bool SafeBlockInfo(const void* ptr, unsigned* out);
// 4-byte read that returns false instead of crashing (another thread may decommit between check and read).
bool SafeRead32(uintptr_t address, uint32_t* out);
bool SafeReadAvailable();  // the exception handler SafeRead32 needs is installed (else SafeRead32 always fails)
// Hang watchdog: call every world tick from the game thread; if it stops for 3 s the
// game thread's EIP and stack code addresses are logged once ("HANG?").
void Heartbeat(unsigned gameThreadId);
void StopWatchdog();
void CrumbFrame(const char* what, unsigned value);
// From DllMain's process-exit detach: logs the exiting thread's stack (the code that called ExitProcess).
void LogProcessExit();
// The MK heap allocation about to run (heap, bytes, caller): named by the out-of-memory log if it fails. Install also
// hooks MK9's out-of-memory handler 0x00406160 (logs; closes the game where MK9 would freeze) and makes a pure virtual
// call close the game instead of the R6025 dialog.
void NoteAlloc(const void* heap, uint32_t size, const void* caller);
// "<tag> stack code values: ..." (MKKE and MK9Rollback code addresses on this thread's stack, up to 32).
void LogStack(const char* tag);
// Diagnostic: arm a 4-byte hardware write breakpoint (DR0) on a thread (0 disarms). Must not be called from that thread.
bool WatchWrite(unsigned threadId, uintptr_t address, bool execute = false);
int WatchWriteAll(uintptr_t address);
extern volatile bool gWatchGated;   // distinct mode: ignore hits while gWatchPhase == 0
extern volatile long gWatchPhase;   // tracer-defined window marker (0 = outside)
extern volatile bool gWatchDistinct;  // log each distinct writer EIP once (up to 12 over 5000 hits)  // write watch on all other threads; returns the number armed
// Diagnostic write trace: up to four 4-byte hardware write watchpoints that stay armed (0 entries unused;
// all 0 disarms). Hits are recorded under the frame set by TraceFrame, live and resimulated passes
// separately; TraceDump(gf) logs where the two passes of frame gf first wrote differently.
bool TraceArm(unsigned threadId, const uintptr_t addrs[4]);
void TraceFrame(uint32_t gf, bool resim);  // gf 0xFFFFFFFF: stop recording (loads, boundaries)
void TraceDump(uint32_t gf);
void TraceLog(uint32_t gf, bool resim, const char* tag);  // every write recorded so far in that pass of frame gf
uintptr_t TraceAddr(int slot);  // the address slot 0..3 is armed on (0: none)
void SetTraceAllThreads(bool on);  // TraceArm also arms every other thread (pool jobs); slot | 0x10 marks their writes
// Change watch: up to four 4-byte hardware write watchpoints (0 entries unused) on every thread, the caller's included
// (armed from a helper thread). Each change of a watched value is logged with its writer ("WATCH <name>: old -> new",
// eip, EBP-chain callers, stack code values): per slot up to 10 changes between zero and nonzero and the first 3 other
// changes; writes that keep the value are not logged. Stays
// armed until ChangeWatchArm with all 0. Names must be string literals.
void ChangeWatchArm(const uintptr_t addrs[4], const char* const names[4]);
// Optional: names the restored-snapshot span containing an address (logged for crash registers).
using RegionNamer = const char* (*)(uint32_t addr, uint32_t* spanBase);
void SetRegionNamer(RegionNamer fn);
// Optional: one-line allocator history of an address (e.g. freed/released frames), for crash dumps.
using PtrHistory = void (*)(uint32_t addr, char* out, unsigned n);
void SetPtrHistory(PtrHistory fn);
// Optional: the dwords the last loaded snapshot holds for [addr, addr + 4n) (false when no span holds all of them);
// crash registers inside a restored span are logged live vs saved.
using SavedDwords = bool (*)(uint32_t addr, uint32_t* out, unsigned n);
void SetSavedDwords(SavedDwords fn);
}
