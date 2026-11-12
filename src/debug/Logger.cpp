#include "Logger.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>

// No file I/O on the caller's thread: Info / Frame format on the caller (stack buffer), take a short lock only to append the
// finished text to an in-memory pending buffer, and return. A below-normal writer thread swaps the pending buffers out and
// does every WriteFile and the frame-log rotation outside that lock, under a separate I/O lock.
//
// Locks, always taken in this order: gIoLock (file handles, the writer's buffers, the frame byte count; held across
// WriteFile) then gPendLock (the pending buffers; never held across I/O). The crash / exit paths (Flush, TryInfo, Close)
// take both with bounded try-locks only: either may be held by a suspended thread, or by one that died holding it.
//
// The files are written with WriteFile on handles opened for appending (no stdio): the bytes are what the CRT's text-mode
// "a" streams wrote before (every '\n' becomes "\r\n"), so the tools see the same files.

#ifndef MK9R_LOG_FRAME_CAP  // a test may build with a smaller cap
#define MK9R_LOG_FRAME_CAP (48LL << 20)
#endif
#ifndef MK9R_LOG_PENDING_FRAME_CAP  // the same
#define MK9R_LOG_PENDING_FRAME_CAP (16u << 20)
#endif

namespace {
// The frame log grows ~2 MB every few minutes; a long online session would fill the disk and the player's
// "Save logs" zip. Past the cap it becomes MK9Rollback_frames.old.log (the previous one dropped).
constexpr long long kFrameLogCap = MK9R_LOG_FRAME_CAP;
// Frame-log text waiting for a stalled disk past this is dropped (counted, reported in the main log on recovery).
// Main-log text is never dropped.
constexpr size_t kPendingFrameCap = MK9R_LOG_PENDING_FRAME_CAP;
constexpr DWORD kWriterPeriodMs = 100;  // frame-log text reaches the disk at most this long after it was logged
constexpr size_t kLineStack = 4096;     // longer lines are formatted on the process heap (Info / Frame) or cut (TryInfo)
constexpr size_t kBufStart = 64u << 10, kShrinkAbove = 4u << 20;  // a buffer's first size; the writer's shrink back to it
// Bounds of the crash / exit paths.
constexpr DWORD kFlushIoMs = 250, kFlushPendMs = 100, kTryInfoPendMs = 200, kTryInfoIoMs = 200;

struct Buf {
    char* p;
    size_t len;
    size_t cap;
    size_t done;  // the writer's buffers: bytes already written
};

bool gInit = false;
CRITICAL_SECTION gIoLock;
CRITICAL_SECTION gPendLock;
HANDLE gHeap = nullptr;  // private, unserialized: only ever used with gPendLock held
HANDLE gWake = nullptr;  // auto-reset; Info sets it, the writer also wakes every kWriterPeriodMs
HANDLE gWriter = nullptr;
DWORD gWriterId = 0;
volatile LONG gWakeFlag = 0, gStop = 0;
volatile LONG gOpen = 0;  // written with both locks held; read without as a fast path, re-checked under gPendLock
volatile LONG gMainOn = 0, gFramesOn = 0;
LARGE_INTEGER gQpf;
volatile LONG gIoMaxMs = 0, gIoSlow = 0;

// gPendLock
Buf gPendMain{}, gPendFrames{};
unsigned gDroppedFrames = 0, gLostMain = 0;

// gIoLock
Buf gOutMain{}, gOutFrames{};
HANDLE gMainH = INVALID_HANDLE_VALUE, gFramesH = INVALID_HANDLE_VALUE;
long long gMainBytes = 0, gFrameBytes = 0;  // the files' sizes as written by us
wchar_t gDir[MAX_PATH] = L"";
// The write in progress (1 main, 2 frames; 0 none), so that a Flush after the writer's death (process exit) finds out
// from the file's size how much of it reached the file, and writes the rest: no line lost, none twice.
volatile LONG gSeg = 0;
long long gSegStart = 0;
size_t gSegDone = 0, gSegLen = 0;
// gDroppedFrames / gLostMain taken with the pending text, until the main log says them (NoteLosses): kept here, not
// in a local of the writer, so that a Flush after its death still says them.
unsigned gNoteDropped = 0, gNoteLost = 0;

using ShutdownFn = BOOLEAN(NTAPI*)();
ShutdownFn gShutdown = nullptr;  // ntdll's RtlDllShutdownInProgress
// The process is exiting (DLL_PROCESS_DETACH after ExitProcess): every other thread is gone, whatever lock it held.
bool ShuttingDown() { return gShutdown && gShutdown(); }

size_t Stamp(char* out) {  // "[HH:MM:SS.mmm] "
    SYSTEMTIME t;
    GetLocalTime(&t);
    return (size_t)snprintf(out, 32, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

struct Line {
    char stack[kLineStack];
    char* heap = nullptr;
    const char* p = stack;
    size_t len = 0;
    ~Line() {
        if (heap) HeapFree(GetProcessHeap(), 0, heap);
    }
};

// The message after `off` bytes already in l.stack. Without mayAlloc a line longer than the stack buffer is cut.
void Format(Line& l, size_t off, bool mayAlloc, const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(l.stack + off, kLineStack - off, fmt, ap);
    if (n < 0) n = 0;
    if (off + (size_t)n < kLineStack) {
        l.len = off + (size_t)n;
    } else if (mayAlloc && (l.heap = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, off + (size_t)n + 1)))) {
        memcpy(l.heap, l.stack, off);
        vsnprintf(l.heap + off, (size_t)n + 1, fmt, ap2);
        l.p = l.heap;
        l.len = off + (size_t)n;
    } else {
        l.len = kLineStack - 1;
    }
    va_end(ap2);
}

size_t CountNl(const char* p, size_t n) {
    size_t k = 0;
    for (const char* e = p + n; (p = static_cast<const char*>(memchr(p, '\n', (size_t)(e - p)))) != nullptr; ++p) ++k;
    return k;
}

// gPendLock held. Stores are ordered so that a reader after this thread's death (Flush at process exit) sees a
// consistent buffer: p before cap, the text before len.
bool Reserve(Buf& b, size_t add, bool mayGrow) {
    if (b.len + add <= b.cap) return true;
    if (!mayGrow) return false;
    size_t nc = b.cap ? b.cap : kBufStart;
    while (nc < b.len + add) nc *= 2;
    char* np = static_cast<char*>(HeapAlloc(gHeap, 0, nc));
    if (!np) return false;
    if (b.len) memcpy(np, b.p, b.len);
    char* old = b.p;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    b.p = np;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    b.cap = nc;
    if (old) HeapFree(gHeap, 0, old);
    return true;
}

// gPendLock held (or its owner dead at process exit: then without growing, the heap may be mid-update);
// text + CRLF, '\n' -> "\r\n".
bool Append(Buf& b, const Line& l, size_t nls, bool mayGrow = true) {
    const size_t need = l.len + nls + 2;
    if (!Reserve(b, need, mayGrow)) return false;
    char* d = b.p + b.len;
    if (!nls) {
        memcpy(d, l.p, l.len);
        d += l.len;
    } else {
        for (size_t i = 0; i < l.len; ++i) {
            if (l.p[i] == '\n') *d++ = '\r';
            *d++ = l.p[i];
        }
    }
    d[0] = '\r';
    d[1] = '\n';
    std::atomic_signal_fence(std::memory_order_seq_cst);
    b.len += need;
    return true;
}

void FreeBuf(Buf& b) {  // gPendLock held
    if (b.p) HeapFree(gHeap, 0, b.p);
    b = Buf{};
}

// gPendLock held; b is empty. A buffer is never left without memory while the log is open: the next swap hands it to
// the callers, and an Info at process exit, after a thread died holding gPendLock, cannot grow it (Append's mayGrow).
// 2026-11-11: the host test's exit children lost both lines of their DLL_PROCESS_DETACH about once in 100 exits on a
// busy PC (a thread killed in the first allocation after a swap: "no memory for the log buffer").
void Shrink(Buf& b) {
    FreeBuf(b);
    Reserve(b, 1, true);
}

void SetClosed() {
    gOpen = 0;
    gMainOn = 0;
    gFramesOn = 0;
}

void Wake() {
    if (InterlockedCompareExchange(&gWakeFlag, 1, 0) == 0 && gWake) SetEvent(gWake);
}

bool TryLockFor(CRITICAL_SECTION* cs, DWORD ms) {
    if (TryEnterCriticalSection(cs)) return true;
    const ULONGLONG end = GetTickCount64() + ms;
    for (int i = 0;; ++i) {
        if (i < 16) SwitchToThread();
        else Sleep(1);
        if (TryEnterCriticalSection(cs)) return true;
        if (GetTickCount64() >= end) return false;
    }
}

// cs (just found taken) is held by a thread that no longer exists: TerminateThread, or process exit, where every other
// thread is gone (one killed inside EnterCriticalSection can leave it taken with no owner recorded). *tid: the owner.
bool OwnerDead(CRITICAL_SECTION* cs, DWORD* tid) {
    const DWORD id = (DWORD)(ULONG_PTR)cs->OwningThread;
    if (tid) *tid = id;
    if (ShuttingDown()) return true;
    if (!id || id == GetCurrentThreadId()) return false;
    HANDLE h = OpenThread(SYNCHRONIZE, FALSE, id);
    if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;  // no such thread
    const bool dead = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return dead;
}

// 1: taken; 0: held by a live thread (*tid), after waiting ms; -1: held by a thread that no longer exists.
int Take(CRITICAL_SECTION* cs, DWORD ms, DWORD* tid) {
    if (ShuttingDown()) return TryEnterCriticalSection(cs) ? 1 : (OwnerDead(cs, tid), -1);  // nobody left to wait for
    if (TryLockFor(cs, ms)) return 1;
    for (int i = 0; i < 3; ++i) {
        if (OwnerDead(cs, tid)) return -1;
        if (*tid) return 0;
        if (TryEnterCriticalSection(cs)) return 1;  // released meanwhile
    }
    return 0;
}

// For Info / Frame: waits while a live thread holds the buffer, as any lock. Returns false (go on without the lock) only
// at process exit, where its holder was killed: waiting there would end the process on the spot (Windows terminates an
// exiting process whose last thread waits for a lock of a thread that is gone), losing every pending line.
bool LockPendForAppend() {
    if (TryEnterCriticalSection(&gPendLock)) return true;
    if (ShuttingDown()) return false;
    for (int i = 0; i < 256; ++i) {
        YieldProcessor();
        if (TryEnterCriticalSection(&gPendLock)) return true;
    }
    EnterCriticalSection(&gPendLock);
    return true;
}

// --- below: gIoLock held, or its owner is dead ---

void NoteIo(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    const LONG ms = (LONG)((t1.QuadPart - t0.QuadPart) * 1000 / gQpf.QuadPart);
    for (LONG m = gIoMaxMs; ms > m && InterlockedCompareExchange(&gIoMaxMs, ms, m) != m; m = gIoMaxMs) {
    }
    if (ms >= 5) InterlockedIncrement(&gIoSlow);
}

void WriteAll(HANDLE h, const char* p, size_t n) {
    while (n && h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        const DWORD k = n > (1u << 30) ? (1u << 30) : (DWORD)n;
        if (!WriteFile(h, p, k, &w, nullptr) || !w) return;
        p += w;
        n -= w;
    }
}

HANDLE OpenIn(const wchar_t* dir, const wchar_t* name) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    // FILE_APPEND_DATA without FILE_WRITE_DATA: every write goes to the end, like the "a" streams before.
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

void RotateFrames() {
    wchar_t cur[MAX_PATH], old[MAX_PATH];
    swprintf(cur, MAX_PATH, L"%ls\\MK9Rollback_frames.log", gDir);
    swprintf(old, MAX_PATH, L"%ls\\MK9Rollback_frames.old.log", gDir);
    CloseHandle(gFramesH);
    MoveFileExW(cur, old, MOVEFILE_REPLACE_EXISTING);
    gFramesH = OpenIn(gDir, L"MK9Rollback_frames.log");
    gFrameBytes = 0;
}

void WriteMain(const char* p, size_t n) {
    WriteAll(gMainH, p, n);
    gMainBytes += (long long)n;
}

// k bytes of the writer's buffer o from o.done, recorded as in progress (see gSeg).
void WriteSeg(LONG sink, HANDLE h, long long& bytes, Buf& o, size_t k) {
    gSegStart = bytes;
    gSegDone = o.done;
    gSegLen = k;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gSeg = sink;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    WriteAll(h, o.p + o.done, k);
    o.done += k;
    bytes += (long long)k;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gSeg = 0;
}

// As before: the line that takes the file past the cap is the old file's last one.
void WriteOutFrames() {
    Buf& o = gOutFrames;
    while (o.done < o.len && gFramesH != INVALID_HANDLE_VALUE) {
        const char* p = o.p + o.done;
        const size_t n = o.len - o.done;
        const size_t room = gFrameBytes < kFrameLogCap ? (size_t)(kFrameLogCap - gFrameBytes) : 0;
        const char* nl = room < n ? static_cast<const char*>(memchr(p + room, '\n', n - room)) : nullptr;
        WriteSeg(2, gFramesH, gFrameBytes, o, nl ? (size_t)(nl - p) + 1 : n);
        if (nl) RotateFrames();
    }
}

void Note(const char* fmt, ...) {  // a main-log line straight to the file, after everything written so far
    Line l;
    const size_t off = Stamp(l.stack);
    va_list ap;
    va_start(ap, fmt);
    Format(l, off, false, fmt, ap);
    va_end(ap);
    if (l.len > kLineStack - 3) l.len = kLineStack - 3;
    l.stack[l.len] = '\r';
    l.stack[l.len + 1] = '\n';
    WriteMain(l.stack, l.len + 2);
}

// The lines that never reached a file, said in the main log after the text they were taken with. A count is cleared
// after its line is written (stores ordered as in Reserve): a thread that dies in here leaves it to the next Flush,
// which may say it twice but not lose it.
void NoteLosses() {
    if (gNoteLost) Note("LOGGER: %u main-log line(s) lost: no memory for the log buffer", gNoteLost);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gNoteLost = 0;
    if (gNoteDropped)
        Note("LOGGER: %u frame-log line(s) dropped: more than %u MB of frame-log text waited for the disk", gNoteDropped,
             (unsigned)(kPendingFrameCap >> 20));
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gNoteDropped = 0;
}

// gPendLock held, or its owner dead: the counts of the pending text go with it.
void TakeLossCounts() {
    gNoteDropped += gDroppedFrames;
    gNoteLost += gLostMain;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gDroppedFrames = gLostMain = 0;
}

// The writer's buffers after their writer died (gIoLock orphaned): what the in-progress write did not get to the file.
void RecoverOut() {
    struct {
        LONG sink;
        Buf* o;
        HANDLE h;
        long long* bytes;
    } s[2] = {{1, &gOutMain, gMainH, &gMainBytes}, {2, &gOutFrames, gFramesH, &gFrameBytes}};
    for (auto& e : s) {
        if (!e.o->len) continue;
        LARGE_INTEGER size;
        if (gSeg == e.sink && GetFileSizeEx(e.h, &size)) {
            long long w = size.QuadPart - gSegStart;
            w = w < 0 ? 0 : w > (long long)gSegLen ? (long long)gSegLen : w;
            e.o->done = gSegDone + (size_t)w;
            *e.bytes = size.QuadPart;
        }
        if (e.o->done < e.o->len) {
            WriteAll(e.h, e.o->p + e.o->done, e.o->len - e.o->done);
            *e.bytes += (long long)(e.o->len - e.o->done);
        }
        e.o->len = e.o->done = 0;
    }
    gSeg = 0;
    NoteLosses();  // what the dead writer had taken with that text
}

// gIoLock and gPendLock held; releases gPendLock before writing.
void SwapAndWrite() {
    if (gOutMain.cap > kShrinkAbove) Shrink(gOutMain);  // after a stall: give the memory back (heap ops need gPendLock)
    if (gOutFrames.cap > kShrinkAbove) Shrink(gOutFrames);
    Buf t = gPendMain;
    gPendMain = gOutMain;
    gOutMain = t;
    t = gPendFrames;
    gPendFrames = gOutFrames;
    gOutFrames = t;
    TakeLossCounts();
    LeaveCriticalSection(&gPendLock);
    LARGE_INTEGER t0;
    if (gOutMain.len) {
        QueryPerformanceCounter(&t0);
        WriteSeg(1, gMainH, gMainBytes, gOutMain, gOutMain.len);
        NoteIo(t0);
        gOutMain.len = gOutMain.done = 0;
    }
    if (gOutFrames.len) {
        QueryPerformanceCounter(&t0);
        WriteOutFrames();
        NoteIo(t0);
        gOutFrames.len = gOutFrames.done = 0;
    }
    NoteLosses();
}

// The pending text straight to the files, without gPendLock: its owner is dead, or we hold it.
void WritePendingDirect() {
    WriteMain(gPendMain.p, gPendMain.len);
    gPendMain.len = 0;
    WriteAll(gFramesH, gPendFrames.p, gPendFrames.len);
    gFrameBytes += (long long)gPendFrames.len;
    gPendFrames.len = 0;
    TakeLossCounts();
    NoteLosses();
}

// Everything pending to the disk, now, on this thread. Never waits longer than ioMs + pendMs for a lock (plus the
// writes themselves).
void DrainSync(DWORD ioMs, DWORD pendMs) {
    if (!gInit || !gOpen) return;
    DWORD tid = 0;
    const int io = Take(&gIoLock, ioMs, &tid);
    if (io == 0) {
        // Alive: the writer in a slow write, or a suspended thread. It writes what it holds, then everything
        // pending, in order; writing pending text from here would put it ahead of that.
        Wake();
        return;
    }
    if (io < 0) {
        if (!gOpen) return;
        RecoverOut();
        if (!ShuttingDown()) Note("LOGGER: thread %lu died holding the log files; the lines waiting in memory follow", tid);
        const int pend = Take(&gPendLock, pendMs, &tid);
        if (pend != 0) WritePendingDirect();
        if (pend > 0) LeaveCriticalSection(&gPendLock);
        return;
    }
    if (gOpen) {
        const int pend = Take(&gPendLock, pendMs, &tid);
        if (pend > 0) {
            SwapAndWrite();
        } else if (pend < 0) {
            WritePendingDirect();
        } else {
            // Read racily for the note only: the text stays in memory and is written when that thread runs again.
            Note("LOGGER: Flush: thread %lu (0: one stopped inside its unlock) has held the log buffer for %lu ms "
                 "(suspended?); %u bytes of main-log and %u bytes of frame-log text wait in memory, written when it runs "
                 "again",
                 tid, pendMs, (unsigned)gPendMain.len, (unsigned)gPendFrames.len);
        }
    }
    LeaveCriticalSection(&gIoLock);
}

DWORD WINAPI Writer(LPVOID) {
    for (;;) {
        WaitForSingleObject(gWake, kWriterPeriodMs);
        InterlockedExchange(&gWakeFlag, 0);  // lines appended from here on wake us again
        const bool stop = gStop != 0;
        // gIoLock then gPendLock; not blocking on gPendLock while holding gIoLock (a crash path may want gIoLock).
        for (;;) {
            EnterCriticalSection(&gIoLock);
            if (TryEnterCriticalSection(&gPendLock)) break;
            LeaveCriticalSection(&gIoLock);
            Sleep(1);
        }
        if (gOpen) SwapAndWrite();
        else LeaveCriticalSection(&gPendLock);
        LeaveCriticalSection(&gIoLock);
        if (stop) return 0;
    }
}

void Emit(bool main, const char* fmt, va_list ap) {
    Line l;
    const size_t off = main ? Stamp(l.stack) : 0;  // on the caller, before the lock: stamps stay in call order
    Format(l, off, true, fmt, ap);
    const size_t nls = CountNl(l.p, l.len);
    const bool locked = LockPendForAppend();
    bool ok = false;
    if (main && gOpen) {
        ok = Append(gPendMain, l, nls, locked);
        if (!ok && !locked) {  // process exit, no room without growing: write out what waits, then there is
            DrainSync(0, 0);
            ok = Append(gPendMain, l, nls, false);
        }
        if (!ok) ++gLostMain;
    } else if (!main && gOpen) {
        if (gPendFrames.len + l.len + nls + 2 > kPendingFrameCap) ++gDroppedFrames;
        else if (!Append(gPendFrames, l, nls, locked)) ++gDroppedFrames;
    }
    if (locked) LeaveCriticalSection(&gPendLock);
    if (ok) {
        if (gWriter) Wake();
        else DrainSync(kFlushIoMs, kFlushPendMs);  // no writer thread could be started
    }
}
}  // namespace

namespace Logger {

bool Open(const wchar_t* directory) {
    if (!gInit) {
        InitializeCriticalSection(&gIoLock);
        InitializeCriticalSection(&gPendLock);
        gHeap = HeapCreate(HEAP_NO_SERIALIZE, 0, 0);
        gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        QueryPerformanceFrequency(&gQpf);
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
            gShutdown = reinterpret_cast<ShutdownFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlDllShutdownInProgress")));
        gInit = true;
    }
    if (gOpen) return gMainOn != 0;
    EnterCriticalSection(&gIoLock);
    wcsncpy(gDir, directory, MAX_PATH - 1);
    gMainH = OpenIn(directory, L"MK9Rollback.log");
    gFramesH = OpenIn(directory, L"MK9Rollback_frames.log");
    gMainBytes = gFrameBytes = 0;
    LARGE_INTEGER size;
    if (gMainH != INVALID_HANDLE_VALUE && GetFileSizeEx(gMainH, &size)) gMainBytes = size.QuadPart;
    if (gFramesH != INVALID_HANDLE_VALUE && GetFileSizeEx(gFramesH, &size)) gFrameBytes = size.QuadPart;
    EnterCriticalSection(&gPendLock);
    gDroppedFrames = gLostMain = 0;
    gMainOn = gMainH != INVALID_HANDLE_VALUE;
    gFramesOn = gFramesH != INVALID_HANDLE_VALUE;
    gOpen = gHeap && (gMainOn || gFramesOn);
    if (!gOpen) {
        gMainOn = 0;
        gFramesOn = 0;
    } else {
        for (Buf* b : {&gPendMain, &gPendFrames, &gOutMain, &gOutFrames}) Reserve(*b, 1, true);  // see Shrink
    }
    LeaveCriticalSection(&gPendLock);
    LeaveCriticalSection(&gIoLock);
    if (gOpen && gHeap && gWake) {
        gStop = 0;
        gWriter = CreateThread(nullptr, 64u << 10, Writer, nullptr, 0, &gWriterId);
        if (gWriter) SetThreadPriority(gWriter, THREAD_PRIORITY_BELOW_NORMAL);
    }
    return gMainOn != 0;
}

void Close() {
    if (!gInit || !gOpen) return;
    InterlockedExchange(&gStop, 1);
    if (gWriter) {
        SetEvent(gWake);
        if (WaitForSingleObject(gWriter, 5000) != WAIT_OBJECT_0) {
            // Stuck in a write for 5 s: the DLL is about to be unmapped under it.
            TerminateThread(gWriter, 1);
            WaitForSingleObject(gWriter, 1000);
        }
        CloseHandle(gWriter);
        gWriter = nullptr;
    }
    DWORD tid = 0;
    const int io = Take(&gIoLock, 1000, &tid);
    if (io == 0) {
        // A Flush stuck in the disk on another thread: stop taking lines, leave the files to it.
        const bool pend = TryLockFor(&gPendLock, 1000);
        SetClosed();
        if (pend) LeaveCriticalSection(&gPendLock);
        return;
    }
    if (io < 0) RecoverOut();  // the writer was terminated in a write
    const int pend = Take(&gPendLock, 1000, &tid);
    if (pend > 0) {
        SetClosed();  // from here Info / Frame / Flush do nothing
        SwapAndWrite();
    } else {
        if (pend < 0) WritePendingDirect();
        else Note("LOGGER: Close: thread %lu holds the log buffer; %u bytes of main-log and %u bytes of frame-log text not "
                  "written", tid, (unsigned)gPendMain.len, (unsigned)gPendFrames.len);
        SetClosed();
    }
    if (gMainH != INVALID_HANDLE_VALUE) CloseHandle(gMainH);
    if (gFramesH != INVALID_HANDLE_VALUE) CloseHandle(gFramesH);
    gMainH = gFramesH = INVALID_HANDLE_VALUE;
    if (TryLockFor(&gPendLock, 1000)) {
        FreeBuf(gPendMain);
        FreeBuf(gPendFrames);
        FreeBuf(gOutMain);
        FreeBuf(gOutFrames);
        LeaveCriticalSection(&gPendLock);
    }
    if (io > 0) LeaveCriticalSection(&gIoLock);
}

void Info(const char* fmt, ...) {
    if (!gMainOn) return;
    va_list ap;
    va_start(ap, fmt);
    Emit(true, fmt, ap);
    va_end(ap);
}

bool TryInfo(const char* fmt, ...) {
    if (!gInit || !gMainOn) return false;
    Line l;
    const size_t off = Stamp(l.stack);
    va_list ap;
    va_start(ap, fmt);
    Format(l, off, false, fmt, ap);  // no heap: a suspended thread may hold its lock
    va_end(ap);
    const size_t nls = CountNl(l.p, l.len);
    DWORD tid = 0;
    const int pend = Take(&gPendLock, kTryInfoPendMs, &tid);
    if (pend == 0 || (pend < 0 && !(gShutdown && gShutdown()))) return false;
    bool ok = gOpen && Append(gPendMain, l, nls, pend > 0);  // owner dead at exit: no lock, no growth
    if (gOpen && !ok && pend < 0) {
        DrainSync(0, 0);
        ok = Append(gPendMain, l, nls, false);
    }
    if (gOpen && !ok) ++gLostMain;
    if (pend > 0) LeaveCriticalSection(&gPendLock);
    if (!ok) return false;
    DrainSync(kTryInfoIoMs, 50);
    return true;
}

void Frame(const char* fmt, ...) {
    if (!gFramesOn) return;
    va_list ap;
    va_start(ap, fmt);
    Emit(false, fmt, ap);
    va_end(ap);
}

void Flush() { DrainSync(kFlushIoMs, kFlushPendMs); }

void IoStats(unsigned* maxMs, unsigned* slow) {
    const LONG m = InterlockedExchange(&gIoMaxMs, 0), s = InterlockedExchange(&gIoSlow, 0);
    if (maxMs) *maxMs = (unsigned)m;
    if (slow) *slow = (unsigned)s;
}

}  // namespace Logger
