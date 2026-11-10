#pragma once
#include <cstdint>

// Two sinks:
//  - MK9Rollback.log         : human-readable events (startup, hooks, errors)
//  - MK9Rollback_frames.log  : one line per instrumented simulation frame
// Both live next to MK9Rollback.dll, never inside the game directory.
//
// No disk I/O on the caller's thread: Info / Frame format the line and append it to memory under a short lock; a
// below-normal writer thread writes it (Info lines within a few ms, Frame lines within ~100 ms). Lines are never
// reordered; per thread they keep call order, and their time stamps are taken at the call.
namespace Logger {
bool Open(const wchar_t* directory);
// Stops the writer, writes everything pending, closes the files. Later calls of every function do nothing.
void Close();
void Info(const char* fmt, ...);
// Info that gives up after ~200 ms if another thread holds the log buffer (a thread that crashed or is suspended inside a
// log call never releases it), then writes everything pending to disk before returning (waiting up to ~200 ms more for
// the file lock). Returns false only when the line could not be queued. For the watchdog's last words before it closes
// the game. Never uses the process heap (a line over 4 KB is cut).
bool TryInfo(const char* fmt, ...);
void Frame(const char* fmt, ...);
// Synchronous drain for crash, exit and unload paths: writes everything pending (both logs) to disk ON THE CALLING THREAD.
// Never deadlocks: it waits at most ~250 ms for the file lock and ~100 ms for the buffer lock (either may be held by a
// suspended or dead thread), then gives up quietly (a note in the main log says what still waits in memory). Not for the
// game thread's per-frame path: the writer thread already flushes.
void Flush();
// Longest single write of the log files (ms) and how many took >= 5 ms since the last call; reset on read.
void IoStats(unsigned* maxMs, unsigned* slow);
}
