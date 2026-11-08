#pragma once
#include <cstdint>

// Two sinks:
//  - MK9Rollback.log         : human-readable events (startup, hooks, errors)
//  - MK9Rollback_frames.log  : one line per instrumented simulation frame
// Both live next to MK9Rollback.dll, never inside the game directory.
namespace Logger {
bool Open(const wchar_t* directory);
void Close();
void Info(const char* fmt, ...);
// Info that gives up after ~200 ms if another thread holds the log (a thread that crashed inside a log call never releases
// it); also flushes the frame log. For the watchdog's last words before it closes the game.
bool TryInfo(const char* fmt, ...);
void Frame(const char* fmt, ...);
void Flush();
}
