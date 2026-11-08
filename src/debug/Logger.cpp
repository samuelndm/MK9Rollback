#include "Logger.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace {
FILE* gMain = nullptr;
FILE* gFrames = nullptr;
// The frame log grows ~2 MB every few minutes; a long online session would fill the disk and the player's
// "Save logs" zip. Past the cap it becomes MK9Rollback_frames.old.log (the previous one dropped).
constexpr long kFrameLogCap = 48L << 20;
long gFrameBytes = 0;
wchar_t gDir[MAX_PATH] = L"";
CRITICAL_SECTION gLock;
bool gLockInit = false;

void Timestamp(char* out, size_t n) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, n, "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

FILE* OpenIn(const wchar_t* dir, const wchar_t* name) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    return _wfopen(path, L"a");
}

void RotateFrames() {  // gLock held
    wchar_t cur[MAX_PATH], old[MAX_PATH];
    swprintf(cur, MAX_PATH, L"%ls\\MK9Rollback_frames.log", gDir);
    swprintf(old, MAX_PATH, L"%ls\\MK9Rollback_frames.old.log", gDir);
    fclose(gFrames);
    MoveFileExW(cur, old, MOVEFILE_REPLACE_EXISTING);
    gFrames = _wfopen(cur, L"a");
    if (gFrames) setvbuf(gFrames, nullptr, _IOFBF, 1 << 16);
    gFrameBytes = 0;
}
}  // namespace

namespace Logger {

bool Open(const wchar_t* directory) {
    if (!gLockInit) {
        InitializeCriticalSection(&gLock);
        gLockInit = true;
    }
    gMain = OpenIn(directory, L"MK9Rollback.log");
    gFrames = OpenIn(directory, L"MK9Rollback_frames.log");
    if (gFrames) {
        setvbuf(gFrames, nullptr, _IOFBF, 1 << 16);
        fseek(gFrames, 0, SEEK_END);
        gFrameBytes = ftell(gFrames);
    }
    wcsncpy(gDir, directory, MAX_PATH - 1);
    return gMain != nullptr;
}

void Close() {
    if (!gLockInit) return;
    EnterCriticalSection(&gLock);
    if (gMain) fclose(gMain);
    if (gFrames) fclose(gFrames);
    gMain = gFrames = nullptr;
    LeaveCriticalSection(&gLock);
}

void Info(const char* fmt, ...) {
    if (!gMain) return;
    char ts[32];
    Timestamp(ts, sizeof ts);
    EnterCriticalSection(&gLock);
    if (!gMain) {  // closed meanwhile (unload)
        LeaveCriticalSection(&gLock);
        return;
    }
    fprintf(gMain, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(gMain, fmt, ap);
    va_end(ap);
    fputc('\n', gMain);
    fflush(gMain);
    LeaveCriticalSection(&gLock);
}

bool TryInfo(const char* fmt, ...) {
    if (!gMain || !gLockInit) return false;
    bool got = false;
    for (int i = 0; i < 20 && !(got = TryEnterCriticalSection(&gLock) != 0); ++i) Sleep(10);
    if (!got) return false;
    if (gMain) {
        char ts[32];
        Timestamp(ts, sizeof ts);
        fprintf(gMain, "[%s] ", ts);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(gMain, fmt, ap);
        va_end(ap);
        fputc('\n', gMain);
        fflush(gMain);
        if (gFrames) fflush(gFrames);
    }
    LeaveCriticalSection(&gLock);
    return true;
}

// Called on the game's main thread every frame: buffered, flushed by Flush().
void Frame(const char* fmt, ...) {
    if (!gFrames) return;
    EnterCriticalSection(&gLock);
    if (!gFrames) {
        LeaveCriticalSection(&gLock);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vfprintf(gFrames, fmt, ap);
    va_end(ap);
    fputc('\n', gFrames);
    if (n > 0) gFrameBytes += n + 1;
    if (gFrameBytes > kFrameLogCap) RotateFrames();
    LeaveCriticalSection(&gLock);
}

void Flush() {
    if (!gLockInit) return;
    EnterCriticalSection(&gLock);
    if (gFrames) fflush(gFrames);
    LeaveCriticalSection(&gLock);
}

}  // namespace Logger
