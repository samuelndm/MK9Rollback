// Stand-alone test of Logger (new) or the HEAD Logger (-DOLD_LOGGER).
#include "Logger.h"

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static LARGE_INTEGER gF;
static inline long long Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static double Us(long long d) { return d * 1e6 / (double)gF.QuadPart; }
static int gFail = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            printf("  FAIL: " __VA_ARGS__);   \
            printf("\n");                     \
            ++gFail;                          \
        }                                     \
    } while (0)

static std::string ReadFile(const std::wstring& p) {
    std::string s;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return s;
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
static std::vector<std::string> Lines(const std::string& s) {  // split on "\r\n"; flags a bare '\n'
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) break;
        size_t e = b;
        if (e > a && s[e - 1] == '\r') --e;
        else v.push_back("<<BARE LF>>");
        v.push_back(s.substr(a, e - a));
        a = b + 1;
    }
    if (a < s.size()) v.push_back("<<PARTIAL>>" + s.substr(a));
    return v;
}
static std::wstring Dir(const wchar_t* base, const wchar_t* name) {
    std::wstring d = std::wstring(base) + L"\\" + name;
    CreateDirectoryW(d.c_str(), nullptr);
    DeleteFileW((d + L"\\MK9Rollback.log").c_str());
    DeleteFileW((d + L"\\MK9Rollback_frames.log").c_str());
    DeleteFileW((d + L"\\MK9Rollback_frames.old.log").c_str());
    return d;
}
static void Stats(const char* what, std::vector<long long>& v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    printf("  %-28s n=%zu median %.2f us  P99 %.2f us  P99.9 %.2f us  max %.1f us\n", what, v.size(), Us(v[v.size() / 2]),
           Us(v[v.size() * 99 / 100]), Us(v[v.size() * 999 / 1000]), Us(v.back()));
}
static const char* Msg(const std::string& l) {  // main-log line without "[HH:MM:SS.mmm] "
    if (l.size() >= 15 && l[0] == '[' && l[3] == ':' && l[6] == ':' && l[9] == '.' && l[13] == ']' && l[14] == ' ')
        return l.c_str() + 15;
    return nullptr;
}

// ---------------- burst: several threads, numbered Info and Frame lines ----------------
constexpr int kThreads = 4, kPerThread = 30000;
struct Burst {
    int id;
    std::vector<long long> info, frame;
};
static DWORD WINAPI BurstThread(LPVOID p) {
    Burst* b = static_cast<Burst*>(p);
    b->info.reserve(kPerThread);
    b->frame.reserve(kPerThread);
    for (int i = 0; i < kPerThread; ++i) {
        long long t0 = Qpc();
        Logger::Info("BURST t%d i%d gf=%u some payload text %08X %s", b->id, i, i * 7u, i * 2654435761u, "abcdefghij");
        long long t1 = Qpc();
        Logger::Frame("FRAME t%d i%d gf=%u rb=%d hash=%08X", b->id, i, i, i % 9, i * 40503u);
        long long t2 = Qpc();
        b->info.push_back(t1 - t0);
        b->frame.push_back(t2 - t1);
    }
    return 0;
}
// Checks: each thread's numbered lines all present, in order. Returns lines seen.
static void CheckNumbered(const std::vector<std::string>& lines, bool main, const char* tag, int threads, int per) {
    std::vector<int> next(threads, 0);
    int bad = 0, other = 0;
    for (auto& l : lines) {
        const char* m = main ? Msg(l) : l.c_str();
        if (!m) {
            ++other;
            continue;
        }
        int t, i;
        char fmt[32];
        snprintf(fmt, sizeof fmt, "%s t%%d i%%d", tag);
        if (sscanf(m, fmt, &t, &i) != 2 || t < 0 || t >= threads) {
            ++other;
            continue;
        }
        if (i != next[t]) {
            if (bad++ < 5) printf("  t%d expected %d got %d\n", t, next[t], i);
        }
        next[t] = i + 1;
    }
    int total = 0;
    for (int t = 0; t < threads; ++t) {
        CHECK(next[t] == per, "%s t%d: last %d of %d", tag, t, next[t], per);
        total += next[t];
    }
    CHECK(!bad, "%s: %d gaps / reorders", tag, bad);
    printf("  %s lines: %d of %d present, in order per thread: %s (%d other lines)\n", tag, total, threads * per,
           bad ? "NO" : "yes", other);
}

static void PhaseBurst(const wchar_t* base) {
    printf("== burst: %d threads x %d Info + %d Frame, a long line, Flush, rotation, Close ==\n", kThreads, kPerThread, kPerThread);
    std::wstring d = Dir(base, L"burst");
    Logger::Open(d.c_str());
    Burst b[kThreads];
    HANDLE h[kThreads];
    long long t0 = Qpc();
    for (int t = 0; t < kThreads; ++t) {
        b[t].id = t;
        h[t] = CreateThread(nullptr, 0, BurstThread, &b[t], 0, nullptr);
    }
    // A long line (longer than the 4 KB stack buffer) in the middle of the burst.
    std::string longText(10000, 'x');
    for (size_t i = 0; i < longText.size(); i += 100) memcpy(&longText[i], "<", 1), longText[i + 1] = '0' + (i / 100) % 10;
    Sleep(5);
    Logger::Info("LONGLINE %s END", longText.c_str());
    Logger::Frame("LONGFRAME %s END", longText.c_str());
    WaitForMultipleObjects(kThreads, h, TRUE, INFINITE);
    long long t1 = Qpc();
    for (int t = 0; t < kThreads; ++t) CloseHandle(h[t]);
    long long f0 = Qpc();
    Logger::Flush();
    long long f1 = Qpc();
    printf("  burst took %.0f ms; Flush() after it %.2f ms\n", Us(t1 - t0) / 1000, Us(f1 - f0) / 1000);
    // Everything must be on disk now, before Close.
    std::string mainLog = ReadFile(d + L"\\MK9Rollback.log");
    std::string fr = ReadFile(d + L"\\MK9Rollback_frames.old.log");
    const size_t oldSize = fr.size();
    std::string cur = ReadFile(d + L"\\MK9Rollback_frames.log");
    fr += cur;
    auto ml = Lines(mainLog), fl = Lines(fr);
    CheckNumbered(ml, true, "BURST", kThreads, kPerThread);
    CheckNumbered(fl, false, "FRAME", kThreads, kPerThread);
    bool longOk = false, longFrameOk = false;
    for (auto& l : ml)
        if (const char* m = Msg(l)) longOk |= std::string(m) == "LONGLINE " + longText + " END";
    for (auto& l : fl) longFrameOk |= l == "LONGFRAME " + longText + " END";
    CHECK(longOk && longFrameOk, "long line");
    printf("  10,000-char Info line intact: %s; Frame line intact: %s\n", longOk ? "yes" : "NO", longFrameOk ? "yes" : "NO");
    int bare = 0;
    for (auto& l : ml) bare += l == "<<BARE LF>>" || l.rfind("<<PARTIAL>>", 0) == 0;
    for (auto& l : fl) bare += l == "<<BARE LF>>" || l.rfind("<<PARTIAL>>", 0) == 0;
    CHECK(!bare, "bare LF / partial lines: %d", bare);
#ifdef MK9R_LOG_FRAME_CAP
    printf("  rotation (cap %lld bytes): frames.old.log %zu bytes, frames.log %zu bytes; old ends on a line: %s\n",
           (long long)MK9R_LOG_FRAME_CAP, oldSize, cur.size(), oldSize && fr[oldSize - 1] == '\n' ? "yes" : "NO");
    CHECK(oldSize > (size_t)MK9R_LOG_FRAME_CAP && oldSize < (size_t)MK9R_LOG_FRAME_CAP + 12000, "rotation size %zu", oldSize);
#else
    (void)oldSize;
#endif
    std::vector<long long> ai, af;
    for (int t = 0; t < kThreads; ++t) {
        ai.insert(ai.end(), b[t].info.begin(), b[t].info.end());
        af.insert(af.end(), b[t].frame.begin(), b[t].frame.end());
    }
    Stats("Info  (4-thread burst)", ai);
    Stats("Frame (4-thread burst)", af);
#ifndef OLD_LOGGER
    unsigned mx = 0, slow = 0;
    Logger::IoStats(&mx, &slow);
    printf("  IoStats: longest write %u ms, %u writes >= 5 ms\n", mx, slow);
#endif
    Logger::Close();
    // After Close: everything harmless.
    Logger::Info("AFTER CLOSE %d", 1);
    Logger::Frame("AFTER CLOSE %d", 2);
    Logger::Flush();
    bool tr = Logger::TryInfo("AFTER CLOSE %d", 3);
    Logger::Close();
    std::string after = ReadFile(d + L"\\MK9Rollback.log") + ReadFile(d + L"\\MK9Rollback_frames.log");
    CHECK(after.find("AFTER CLOSE") == std::string::npos && !tr, "calls after Close");
    printf("  calls after Close: no output, TryInfo returned %d, no crash\n", (int)tr);
}

// ---------------- paced: a game-thread-like 60 Hz loop while another thread logs ----------------
static volatile LONG gStopBg = 0;
static DWORD WINAPI Background(LPVOID) {
    int i = 0;
    while (!gStopBg) {
        Logger::Info("BG line %d with some text to make it a typical length, values %08X %08X", i, i * 3u, i * 5u), ++i;
        Sleep(0);
    }
    return 0;
}
static void PhasePaced(const wchar_t* base) {
    printf("== paced: 600 ticks at ~2 ms, each 2 Info + 30 Frame, one background thread logging Info flat out ==\n");
    std::wstring d = Dir(base, L"paced");
    Logger::Open(d.c_str());
    HANDLE bg = CreateThread(nullptr, 0, Background, nullptr, 0, nullptr);
    std::vector<long long> vi, vf, tick;
    for (int k = 0; k < 600; ++k) {
        long long k0 = Qpc();
        for (int j = 0; j < 32; ++j) {
            long long t0 = Qpc();
            if (j < 2) Logger::Info("TICK %d info %d: something happened at gf=%d", k, j, k * 3);
            else Logger::Frame("TICK %d frame %d gf=%d rb=%d hash %08X %08X", k, j, k, j % 7, k * 13u, j * 17u);
            long long t1 = Qpc();
            (j < 2 ? vi : vf).push_back(t1 - t0);
        }
        tick.push_back(Qpc() - k0);
        Sleep(2);
    }
    gStopBg = 1;
    WaitForSingleObject(bg, INFINITE);
    CloseHandle(bg);
    Stats("Info  (paced, contended)", vi);
    Stats("Frame (paced, contended)", vf);
    Stats("32 calls per tick", tick);
    Logger::Close();
}

// ---------------- format: the bytes against the HEAD logger ----------------
static void PhaseFormat(const wchar_t* base) {
    std::wstring d = Dir(base, L"format");
    Logger::Open(d.c_str());
    Logger::Info("hello %d %s", 42, "world");
    Logger::Info("two\nlines and a \r\n crlf");
    Logger::Info("%s", "");
    Logger::Frame("frame %u", 7u);
    Logger::Frame("f\nm");
    Logger::Frame("%s", "");
    Logger::Close();
}

// ---------------- suspend: Flush / TryInfo with threads suspended mid-burst ----------------
static volatile LONG gStopS = 0, gPaced = 0;
static DWORD WINAPI SusThread(LPVOID p) {
    const int id = (int)(intptr_t)p;
    for (int i = 0; !gStopS; ++i) {
        Logger::Info("SUS t%d i%d payload %08X %08X %08X", id, i, i * 3u, i * 5u, i * 7u);
        if (!(i & 3)) Logger::Frame("SUSF t%d i%d", id, i);
        if (gPaced && i % 20 == 19) Sleep(1);
    }
    return 0;
}
// Streams a CRLF log line by line (the flat-out suspend run writes too much to hold in a 32-bit process).
template <class F> static int ForEachLine(const std::wstring& p, F f) {
    FILE* fp = _wfopen(p.c_str(), L"rb");
    if (!fp) return -1;
    static char buf[1 << 16];
    int malformed = 0;
    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        if (n < 2 || buf[n - 1] != '\n' || buf[n - 2] != '\r') {
            ++malformed;
            continue;
        }
        buf[n - 2] = 0;
        f(std::string(buf, n - 2));
    }
    fclose(fp);
    return malformed;
}
static void PhaseSuspend(const wchar_t* base) {
    printf("== suspend: 3 threads in an Info burst; 200 rounds: suspend (odd rounds: ALL other threads, writer included;\n"
           "   even rounds: one burst thread), then Flush() and TryInfo() on this thread, then resume ==\n");
    std::wstring d = Dir(base, L"suspend");
    Logger::Open(d.c_str());
    const int kT = 3;
    HANDLE th[kT];
    DWORD ids[kT];
    for (int t = 0; t < kT; ++t) th[t] = CreateThread(nullptr, 0, SusThread, (LPVOID)(intptr_t)t, 0, &ids[t]);
    Sleep(20);
    std::vector<long long> fl, ti;
    std::vector<int> tryOk;
    srand(1);
    for (int r = 0; r < 200; ++r) {
        if (r == 100) gPaced = 1;  // rounds 0-99: the threads log flat out; 100-199: 20 lines per ms
        Sleep(1 + rand() % 4);
        std::vector<HANDLE> sus;
        if (r & 1) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            std::vector<DWORD> tids;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                    tids.push_back(te.th32ThreadID);
            CloseHandle(snap);
            sus.reserve(tids.size());
            for (DWORD id : tids) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, id);
                if (h && SuspendThread(h) != (DWORD)-1) sus.push_back(h);
                else if (h) CloseHandle(h);
            }
        } else {
            sus.push_back(th[r % kT]);
            SuspendThread(th[r % kT]);
        }
        // Nothing here may take a lock a suspended thread could hold (no printf, no heap).
        long long a = Qpc();
        Logger::Flush();
        long long b = Qpc();
        const bool ok = Logger::TryInfo("TRY r%d", r);
        long long c = Qpc();
        fl.push_back(b - a);
        ti.push_back(c - b);
        tryOk.push_back(ok);
        for (HANDLE h : sus) ResumeThread(h);
        if (r & 1)
            for (HANDLE h : sus) CloseHandle(h);
    }
    gStopS = 1;
    WaitForMultipleObjects(kT, th, TRUE, INFINITE);
    for (int t = 0; t < kT; ++t) CloseHandle(th[t]);
    Logger::Flush();
    Stats("Flush() while suspended", fl);
    Stats("TryInfo() while suspended", ti);
    int nok = 0;
    for (int v : tryOk) nok += v;
    int over200 = 0, mid = 0, tover = 0;
    for (long long v : fl) over200 += Us(v) > 200000, mid += Us(v) > 90000 && Us(v) <= 200000;
    for (long long v : ti) tover += Us(v) > 150000;
    printf("  TryInfo returned true %d of 200 times; Flush gave up on the file lock (writer suspended mid-write) %d times,\n"
           "  on the buffer lock (a suspended thread inside Info) %d times; TryInfo waited > 150 ms %d times\n",
           nok, over200, mid, tover);
    // Every thread's lines contiguous and in order; TRY lines present iff TryInfo returned true, in order.
    std::vector<int> next(kT, 0);
    int bad = 0, notes = 0, tryNext = 0, tryBad = 0, total = 0;
    int malformed = ForEachLine(d + L"\\MK9Rollback.log", [&](const std::string& l) {
        const char* m = Msg(l);
        int t, i, r;
        if (!m) {
            ++bad;
            return;
        }
        if (sscanf(m, "SUS t%d i%d", &t, &i) == 2) {
            if (i != next[t] && bad++ < 5) printf("  t%d expected %d got %d\n", t, next[t], i);
            next[t] = i + 1;
            ++total;
        } else if (sscanf(m, "TRY r%d", &r) == 1) {
            while (tryNext < r && !tryOk[tryNext]) ++tryNext;
            if (r != tryNext || !tryOk[r]) ++tryBad;
            tryNext = r + 1;
        } else if (!strncmp(m, "LOGGER:", 7)) {
            if (notes++ < 3) printf("  note: %.200s\n", m);
        } else {
            ++bad;
        }
    });
    bad += malformed;
    while (tryNext < 200 && !tryOk[tryNext]) ++tryNext;
    CHECK(!bad && !tryBad && tryNext == 200, "suspend log: %d bad, %d try bad, tryNext %d", bad, tryBad, tryNext);
    // Frame lines too.
    std::vector<int> fnext(kT, -1);  // from each thread's first line: the run may rotate the (test-sized) frame log
    int fbad = 0, ftotal = 0;
    auto fcheck = [&](const std::string& l) {
        int t, i;
        if (sscanf(l.c_str(), "SUSF t%d i%d", &t, &i) != 2 || t < 0 || t >= kT) {
            ++fbad;
            return;
        }
        if (fnext[t] >= 0 && i != fnext[t]) ++fbad;
        fnext[t] = i + 4;
        ++ftotal;
    };
    ForEachLine(d + L"\\MK9Rollback_frames.old.log", fcheck);
    fbad += ForEachLine(d + L"\\MK9Rollback_frames.log", fcheck);
    CHECK(!fbad, "suspend frame log: %d bad", fbad);
    printf("  %d SUSF frame lines, per thread contiguous and in order: %s\n", ftotal, fbad ? "NO" : "yes");
    printf("  %d SUS lines, per thread contiguous and in order: %s; TRY lines as returned: %s; %d LOGGER notes\n", total,
           bad ? "NO" : "yes", tryBad || tryNext != 200 ? "NO" : "yes", notes);
    Logger::Close();
}

// ---------------- child: log, Flush, TerminateProcess ----------------
static int Child(const wchar_t* dir, int n, bool flush, bool paced = false) {
    Logger::Open(dir);
    for (int i = 0; i < n; ++i) {
        Logger::Info("CHILD i%d", i);
        Logger::Frame("CHILDF i%d", i);
        if (paced)  // one Info + one Frame per ms, busy-waiting: a hard kill shows how many ms of lines are lost
            for (long long t = Qpc(); Us(Qpc() - t) < 1000;) {
            }
    }
    if (flush) Logger::Flush();
    TerminateProcess(GetCurrentProcess(), 7);
    return 0;
}
// ---------------- child: ExitProcess while threads log; Flush in DLL_PROCESS_DETACH (a TLS callback) ----------------
static volatile LONG gExitChild = 0, gDone[3] = {-1, -1, -1};
// ExitProcess ends the other threads without waiting for them: one that is running on another processor goes on for a
// moment. Counted here: the rounds a logging thread began with the process's shutdown flag already set.
static BOOLEAN(NTAPI* gShutdownFn)() = nullptr;  // ntdll's RtlDllShutdownInProgress
static volatile LONG gLateRounds = 0;
static DWORD WINAPI ExitLogger(LPVOID p) {
    const int id = (int)(intptr_t)p;
    for (int i = 0;; ++i) {
        if (gShutdownFn && gShutdownFn()) InterlockedIncrement(&gLateRounds);
        Logger::Info("EX t%d i%d payload %08X %08X", id, i, i * 3u, i * 5u);
        Logger::Frame("EXF t%d i%d", id, i);
        gDone[id] = i;  // Info line i returned
    }
}
static wchar_t gMarkPath[MAX_PATH];
static void Mark(const char* s) {  // raw progress marker, no Logger, no CRT
    if (!gMarkPath[0]) return;
    HANDLE f = CreateFileW(gMarkPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    DWORD w;
    WriteFile(f, s, lstrlenA(s), &w, nullptr);
    CloseHandle(f);
}
static void NTAPI TlsCb(PVOID, DWORD reason, PVOID) {
    if (reason != DLL_PROCESS_DETACH || !gExitChild) return;
    Mark("cb ");
    long long a = Qpc();
    const LONG late = gLateRounds;
    Logger::Info("DETACH: the other threads are gone; done %ld %ld %ld; rounds begun after the shutdown flag: %ld", gDone[0], gDone[1],
                 gDone[2], late);
    Mark("info ");
    Logger::Flush();
    Mark("flush ");
    long long b = Qpc();
    Logger::Info("DETACH: Info+Flush took %.0f us", Us(b - a));
    Logger::Flush();
    char m[40] = "late ", *e = m + 5;  // the same count for a child whose DETACH line is not in the log (no CRT here)
    char digits[12];
    int n = 0;
    for (unsigned long v = (unsigned long)(late < 0 ? 0 : late); n == 0 || v; v /= 10) digits[n++] = (char)('0' + v % 10);
    while (n) *e++ = digits[--n];
    *e++ = '\r';
    *e++ = '\n';
    *e = 0;
    Mark(m);
}
extern "C" __attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK gTlsCb = TlsCb;
static int ChildExit(const wchar_t* dir, int ms) {
    gExitChild = 1;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        gShutdownFn = reinterpret_cast<BOOLEAN(NTAPI*)()>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlDllShutdownInProgress")));
    swprintf(gMarkPath, MAX_PATH, L"%ls\\mark.txt", dir);
    Logger::Open(dir);
    for (int t = 0; t < 3; ++t) CreateThread(nullptr, 0, ExitLogger, (LPVOID)(intptr_t)t, 0, nullptr);
    Sleep(ms);
    ExitProcess(9);
}
// Frame-log text is dropped, by design, while more of it waits for the disk than the logger's cap (a stalled or starved
// writer; the main log says how many lines). Children that had such lines, over all rounds.
static int gDropChildren = 0;
// Children whose exit came while the writer was inside a rotation of the frame log (the main log says so): the Flush in
// DLL_PROCESS_DETACH then finds the file closed and has to open it again for the lines still in memory. About 1 exit in
// 700 at the suite's 3 MB cap, a third of them at 32 KB. Over all rounds.
static int gRotateChildren = 0;
static const char kRotationNote[] = "LOGGER: the frame log was closed for its rotation when the thread writing it died; opened again";
static void PhaseExit(const wchar_t* base, const wchar_t* exe) {
    printf("== 60 children: 3 threads log flat out, ExitProcess at a random point, Info+Flush in DLL_PROCESS_DETACH ==\n");
    int ok = 0, gaps = 0, missing = 0, maxUs = 0, lostChildren = 0, dropChildren = 0, lateChildren = 0, rotateChildren = 0;
    long long lines = 0, droppedLines = 0, lateRounds = 0;
    static int kept = 0;
    for (int k = 0; k < 60; ++k) {
        std::wstring d = Dir(base, L"childexit");
        DeleteFileW((d + L"\\mark.txt").c_str());
        wchar_t cmd[1024];
        swprintf(cmd, 1024, L"\"%ls\" childexit \"%ls\" %d", exe, d.c_str(), 20 + k * 4);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
            CHECK(false, "CreateProcess");
            return;
        }
        if (WaitForSingleObject(pi.hProcess, 20000) != WAIT_OBJECT_0) {
            CHECK(false, "child %d hung in exit", k);
            TerminateProcess(pi.hProcess, 1);
        }
        DWORD xc = 0;
        GetExitCodeProcess(pi.hProcess, &xc);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        int next[3] = {0, 0, 0}, done[3] = {-2, -2, -2};
        bool sawDetach = false, inRotation = false;
        int us = -1, bad = 0, late = 0;
        long long dropped = 0;  // frame lines the main log reports as dropped
        unsigned lostNotes = 0; // main-log lines it reports as lost
        const int malformed = ForEachLine(d + L"\\MK9Rollback.log", [&](const std::string& l) {
            const char* m = Msg(l);
            int t, i, end = 0;
            unsigned n;
            if (!m) return (void)++bad;
            if (sscanf(m, "EX t%d i%d", &t, &i) == 2 && t >= 0 && t < 3) {
                if (i != next[t]) ++bad;
                next[t] = i + 1;
                ++lines;
            } else if (sscanf(m, "DETACH: the other threads are gone; done %d %d %d; rounds begun after the shutdown flag: %d", &done[0],
                              &done[1], &done[2], &late) >= 3) {
                sawDetach = true;
            } else if (sscanf(m, "DETACH: Info+Flush took %d us", &us) == 1) {
            } else if (sscanf(m, "LOGGER: %u frame-log line(s) dropped%n", &n, &end) == 1 && end) {
                dropped += n;
            } else if (sscanf(m, "LOGGER: %u main-log line(s) lost%n", &n, &end) == 1 && end) {
                lostNotes += n;
            } else if (!strncmp(m, kRotationNote, sizeof kRotationNote - 1)) {
                inRotation = true;
            } else if (strncmp(m, "LOGGER:", 7)) {
                ++bad;
            } else if (k < 3 || bad) {
                printf("  child %d note: %.160s\n", k, m);
            }
        });
        int fnext[3] = {-1, -1, -1};  // from each thread's first line: the test-sized cap rotates the frame log
        // Frame lines that are not in the files: skipped numbers inside a thread's lines, and its last completed ones.
        // Each must be one the main log reports as dropped (the report may count more: lines of a file rotated away,
        // and a line a thread was killed in).
        long long absent = 0;
        auto fcheck = [&](const std::string& l) {
            int t, i;
            if (sscanf(l.c_str(), "EXF t%d i%d", &t, &i) != 2 || t < 0 || t >= 3 || (fnext[t] >= 0 && i < fnext[t]))
                return (void)++bad;
            if (fnext[t] >= 0) absent += i - fnext[t];
            fnext[t] = i + 1;
        };
        ForEachLine(d + L"\\MK9Rollback_frames.old.log", fcheck);
        ForEachLine(d + L"\\MK9Rollback_frames.log", fcheck);
        for (int t = 0; t < 3; ++t)
            if (sawDetach && fnext[t] <= done[t]) absent += done[t] + 1 - (fnext[t] < 0 ? 0 : fnext[t]);
        if (absent > dropped) {
            printf("  child %d: %lld frame lines are not in the files, the main log reports %lld dropped\n", k, absent, dropped);
            ++bad;
        }
        if (lostNotes) {
            printf("  child %d: the main log reports %u of its own lines lost\n", k, lostNotes);
            bad += (int)lostNotes;
        }
        dropChildren += dropped > 0;
        rotateChildren += inRotation;
        droppedLines += dropped;
        gaps += bad;
        if (!sawDetach) printf("  child %d: no DETACH line, exit code %08lX, main log %d EX lines\n", k, xc, next[0] + next[1] + next[2]);
        missing += !sawDetach;
        if (us > maxUs) maxUs = us;
        int lost = 0;
        for (int t = 0; t < 3; ++t) lost += sawDetach && next[t] <= done[t];
        if (lost && lostChildren++ < 3) printf("  child %d: lines missing: done %d/%d/%d, on disk to %d/%d/%d\n", k, done[0], done[1], done[2], next[0] - 1, next[1] - 1, next[2] - 1);
        // A line that is not text + CRLF (a NUL inside, a cut last line) is not read above: it counts.
        if (malformed > 0) {
            printf("  child %d: %d line(s) of the main log are not text ending in CR LF\n", k, malformed);
            bad += malformed;
            gaps += malformed;
        }
        ok += sawDetach && !bad && !lost;
        lateChildren += late > 0;
        lateRounds += late;
        if ((!sawDetach || bad || lost) && kept < 4) {  // a failed child's files stay, with the marks of its exit
            wchar_t to[MAX_PATH];
            swprintf(to, MAX_PATH, L"%ls\\failed%d", base, kept++);
            CreateDirectoryW(to, nullptr);
            for (const wchar_t* f : {L"MK9Rollback.log", L"MK9Rollback_frames.log", L"MK9Rollback_frames.old.log", L"mark.txt"})
                CopyFileW((d + L"\\" + f).c_str(), (std::wstring(to) + L"\\" + f).c_str(), FALSE);
            printf("  child %d (lived %d ms): files kept in %ls; exit marks: %s\n", k, 20 + k * 4, to,
                   ReadFile(d + L"\\mark.txt").c_str());
        }
    }
    CHECK(!gaps && !missing && !lostChildren, "exit children: %d gaps, %d without the detach line, %d lost lines", gaps, missing, lostChildren);
    printf("  %d of 60 exits: every line each thread completed, in order, plus the DETACH line on disk (%lld lines); longest Info+Flush in\n"
           "  DLL_PROCESS_DETACH %d us; children that hung: none if no FAIL above\n", ok, lines, maxUs);
    printf("  frame-log lines dropped over the cap on text waiting for the disk: %lld in %d of 60 children, every absent line reported\n",
           droppedLines, dropChildren);
    printf("  threads still logging after the shutdown flag was set: %lld rounds in %d of 60 children\n", lateRounds, lateChildren);
    printf("  exits inside a rotation of the frame log: %d of 60 children, the file opened again for the lines in memory\n", rotateChildren);
    gDropChildren += dropChildren;
    gRotateChildren += rotateChildren;
}
static void PhaseChild(const wchar_t* base, const wchar_t* exe, const wchar_t* mode) {
    const bool flush = !wcscmp(mode, L"child"), paced = !wcscmp(mode, L"childpaced");
    const int n = paced ? 500 : 20000;
    std::wstring d = Dir(base, mode);
    wchar_t cmd[1024];
    swprintf(cmd, 1024, L"\"%ls\" %ls \"%ls\" %d", exe, mode, d.c_str(), n);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
        CHECK(false, "CreateProcess");
        return;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    auto ml = Lines(ReadFile(d + L"\\MK9Rollback.log")), fl = Lines(ReadFile(d + L"\\MK9Rollback_frames.log"));
    int mi = 0, fi = 0;
    for (auto& l : ml)
        if (const char* m = Msg(l)) mi += !strncmp(m, "CHILD i", 7);
    for (auto& l : fl) fi += !strncmp(l.c_str(), "CHILDF i", 8);
    printf("  child %s TerminateProcess (exit %lu): %d of %d Info and %d of %d Frame lines on disk\n",
           flush ? "Flush() then" : paced ? "1 Info+1 Frame per ms, no Flush, then" : "burst, no Flush, then", code, mi, n, fi, n);
    if (flush) CHECK(code == 7 && mi == n && fi == n, "child lines");
}

// ---------------- closed: the open that ends a rotation fails ----------------
// A rotation renames the frame log and opens a new one. A second thread takes the name in between, as a directory, so
// that the open fails (in a build with a small MK9R_LOG_FRAME_CAP: the log rotates every few ms, the thread gets there
// within a few rotations). The log then says so, counts the lines it drops, and opens the file again once the name is
// free: every line missing from the files must be in a count, and every line after the reopening on disk.
static wchar_t gTakePath[MAX_PATH];
static volatile LONG gTaken = 0, gStopTake = 0;
static DWORD WINAPI TakeName(LPVOID) {
    for (;;) {
        if (gStopTake) return 0;
        if (CreateDirectoryW(gTakePath, nullptr)) break;
    }
    gTaken = 1;
    return 0;
}
static void PhaseClosed(const wchar_t* base) {
    printf("== closed: the frame log's name is taken between a rotation's rename and its open; then given back ==\n");
    std::wstring d = Dir(base, L"closed");
    swprintf(gTakePath, MAX_PATH, L"%ls\\MK9Rollback_frames.log", d.c_str());
    RemoveDirectoryW(gTakePath);  // what a stopped run left
    Logger::Open(d.c_str());
    gTaken = 0;
    gStopTake = 0;
    HANDLE th = CreateThread(nullptr, 0, TakeName, nullptr, 0, nullptr);
    int i = 0;
    const long long t0 = Qpc();
    // 4000 lines (a file and more) per Sleep(1): at least one rotation each time, and far below what the writer writes,
    // so no line is dropped for waiting (that has its own test).
    while (!gTaken && Us(Qpc() - t0) < 30e6) {
        for (int k = 0; k < 4000; ++k) Logger::Frame("CL i%d", i++);
        Sleep(1);
    }
    gStopTake = 1;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    const int atTaken = i;
    const double takenMs = Us(Qpc() - t0) / 1000;
    if (!gTaken) {
        CHECK(false, "closed: the frame log's name was never free for the other thread in 30 s (%d lines): not exercised", i);
        Logger::Close();
        return;
    }
    for (int k = 0; k < 3000; ++k) Logger::Frame("CL i%d", i++);
    Logger::Flush();  // the open fails again; this says the count so far
    bool freed = false;
    for (int k = 0; k < 200 && !(freed = RemoveDirectoryW(gTakePath) != 0); ++k) Sleep(5);
    CHECK(freed, "closed: the directory could not be removed, error %lu", GetLastError());
    for (int k = 0; k < 500; ++k) Logger::Frame("CL i%d", i++);  // less than a file: no further rotation
    Logger::Flush();
    Logger::Close();

    int failed = 0, again = 0, counts = 0, other = 0;
    unsigned long err = 0;
    long long reported = 0, waitDropped = 0;
    const int malformed = ForEachLine(d + L"\\MK9Rollback.log", [&](const std::string& l) {
        const char* m = Msg(l);
        unsigned n;
        unsigned long e;
        int end = 0;
        if (!m) return (void)++other;
        if (sscanf(m, "LOGGER: the frame log cannot be opened after its rotation (error %lu)%n", &e, &end) == 1 && end) {
            ++failed, err = e;
        } else if (sscanf(m, "LOGGER: the frame log is closed (error %lu at its last open): %u frame-log line(s) dropped%n", &e, &n, &end) == 2 && end) {
            reported += n, ++counts;
        } else if (sscanf(m, "LOGGER: the frame log is open again; %u frame-log line(s) were dropped%n", &n, &end) == 1 && end) {
            reported += n, ++again;
        } else if (sscanf(m, "LOGGER: %u frame-log line(s) dropped%n", &n, &end) == 1 && end) {
            waitDropped += n;
        } else {
            if (other++ < 3) printf("  closed: unexpected main-log line: %.160s\n", m);
        }
    });
    // The file the failed rotation completed, then the one opened after the name was free again.
    long long first[2] = {-1, -1}, last[2] = {-1, -1};
    int bad = 0;
    const wchar_t* names[2] = {L"\\MK9Rollback_frames.old.log", L"\\MK9Rollback_frames.log"};
    for (int f = 0; f < 2; ++f) {
        const int r = ForEachLine(d + names[f], [&](const std::string& l) {
            int v;
            if (sscanf(l.c_str(), "CL i%d", &v) != 1 || (last[f] >= 0 && v != last[f] + 1)) return (void)++bad;
            if (first[f] < 0) first[f] = v;
            last[f] = v;
        });
        bad += r < 0 ? 1 : r;
    }
    const long long missing = first[1] - last[0] - 1;
    CHECK(failed == 1 && again == 1, "closed: %d lines say the open failed, %d that the file is open again (1 and 1 wanted)", failed, again);
    CHECK(!bad && !other && !malformed, "closed: %d bad frame-log lines, %d unexpected and %d malformed main-log lines", bad, other, malformed);
    CHECK(last[0] >= 0 && first[1] > last[0] && last[0] < atTaken + 3000 && first[1] >= atTaken + 3000,
          "closed: old file ends at line %lld, new one begins at %lld; the name was taken near line %d", last[0], first[1], atTaken);
    CHECK(last[1] == i - 1, "closed: the new file ends at line %lld, the last line logged was %d", last[1], i - 1);
    // Lines dropped for waiting (none expected) are in neither count's range for certain: they only widen the check.
    CHECK(reported <= missing && missing <= reported + waitDropped,
          "closed: %lld lines are in neither file, the main log reports %lld dropped while the file was closed (+ %lld for waiting)",
          missing, reported, waitDropped);
    printf("  a rotation's open failed (error %lu, name taken after %d lines, %.0f ms): %lld lines in neither file, %lld reported in %d\n"
           "  main-log lines; the file open again once the name was free, lines %lld..%lld on disk\n",
           err, atTaken, takenMs, missing, reported, counts + again, first[1], last[1]);
}

static int CmpFormat(const wchar_t* a, const wchar_t* b) {
    for (const wchar_t* f : {L"\\format\\MK9Rollback.log", L"\\format\\MK9Rollback_frames.log"}) {
        std::string x = ReadFile(std::wstring(a) + f), y = ReadFile(std::wstring(b) + f);
        for (std::string* s : {&x, &y})  // time stamps differ: blank the digits of "[HH:MM:SS.mmm]"
            for (size_t i = 0; i + 14 < s->size(); ++i)
                if ((*s)[i] == '[' && (*s)[i + 13] == ']' && (*s)[i + 3] == ':')
                    for (size_t k = 1; k < 13; ++k)
                        if ((*s)[i + k] >= '0' && (*s)[i + k] <= '9') (*s)[i + k] = '0';
        printf("  %ls: %zu vs %zu bytes, %s\n", f + 8, x.size(), y.size(), x == y ? "IDENTICAL" : "DIFFERENT");
        if (x != y) ++gFail;
        printf("    bytes:");
        for (unsigned char c : x) printf(c >= 32 && c < 127 ? "%c" : "<%02X>", c);
        printf("\n");
    }
    return gFail;
}

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&gF);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 4 && !wcscmp(argv[1], L"child")) return Child(argv[2], _wtoi(argv[3]), true);
    if (argc >= 4 && !wcscmp(argv[1], L"childnoflush")) return Child(argv[2], _wtoi(argv[3]), false);
    if (argc >= 4 && !wcscmp(argv[1], L"childexit")) return ChildExit(argv[2], _wtoi(argv[3]));
    if (argc >= 4 && !wcscmp(argv[1], L"childpaced")) return Child(argv[2], _wtoi(argv[3]), false, true);
    if (argc >= 4 && !wcscmp(argv[1], L"cmpfmt")) return CmpFormat(argv[2], argv[3]);
    if (argc < 2) return 2;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* base = argv[1];
    CreateDirectoryW(base, nullptr);
#ifdef OLD_LOGGER
    printf("##### HEAD Logger.cpp #####\n");
#else
    printf("##### new Logger.cpp #####\n");
#endif
    if (argc >= 3 && !wcscmp(argv[2], L"closedonly")) {  // closedonly [rounds], in a build with a small MK9R_LOG_FRAME_CAP
        const int rounds = argc >= 4 ? _wtoi(argv[3]) : 1;
        for (int r = 0; r < rounds; ++r) PhaseClosed(base);
        printf("%s (%d failures)\n", gFail ? "FAILED" : "ALL CHECKS PASSED", gFail);
        return gFail;
    }
    if (argc >= 3 && !wcscmp(argv[2], L"exitonly")) {  // exitonly [rounds] [drops | rotations]
        const int rounds = argc >= 4 ? _wtoi(argv[3]) : 5;
        for (int r = 0; r < rounds; ++r) PhaseExit(base, exe);
        // A build with a small MK9R_LOG_PENDING_FRAME_CAP: the drop report itself is under test.
        if (argc >= 5 && !wcscmp(argv[4], L"drops")) CHECK(gDropChildren > 0, "no child dropped a frame-log line: the drop report was not exercised");
        // A build with a small MK9R_LOG_FRAME_CAP: the exit inside a rotation is under test, and the rotation whose open fails.
        if (argc >= 5 && !wcscmp(argv[4], L"rotations")) {
            CHECK(gRotateChildren > 0, "no child exited inside a rotation of the frame log: that exit was not exercised");
            PhaseClosed(base);
        }
        printf("%s (%d failures)\n", gFail ? "FAILED" : "ALL CHECKS PASSED", gFail);
        return gFail;
    }
    PhaseFormat(base);
    PhaseBurst(base);
    PhasePaced(base);
#ifndef OLD_LOGGER
    PhaseSuspend(base);
#endif
    printf("== child processes ==\n");
    PhaseChild(base, exe, L"child");
    PhaseChild(base, exe, L"childnoflush");
    PhaseChild(base, exe, L"childpaced");
    PhaseExit(base, exe);

    printf("%s (%d failures)\n", gFail ? "FAILED" : "ALL CHECKS PASSED", gFail);
    return gFail;
}
