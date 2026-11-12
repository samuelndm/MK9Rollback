// src\common\StrAppend.h: AppendF never writes past its buffer, always leaves it terminated, and returns a length the
// next call (and a closing `buf[n] = 0`) can use, whatever came before. The pattern it replaced,
// `n += snprintf(buf + n, sizeof buf - n, ...)`, is run here too, on a buffer with guard bytes behind it, to show what
// it did once a line was cut.
#include <cstdio>
#include <cstring>

#include "../../src/common/StrAppend.h"

namespace {
int gFailed = 0, gChecks = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s\n", what);
}

struct Guarded {
    char buf[16];
    char guard[64];
    Guarded() {
        memset(buf, 'x', sizeof buf);
        memset(guard, '#', sizeof guard);
    }
    bool Whole() const {
        for (char c : guard)
            if (c != '#') return false;
        return true;
    }
};
}  // namespace

int main() {
    {  // plain appends
        Guarded g;
        int n = 0;
        n = AppendF(g.buf, sizeof g.buf, n, "%s", "abc");
        n = AppendF(g.buf, sizeof g.buf, n, " %d", 42);
        Check(n == 6 && !strcmp(g.buf, "abc 42") && g.Whole(), "two appends that fit");
    }
    {  // exactly full: 15 characters and the terminator
        Guarded g;
        const int n = AppendF(g.buf, sizeof g.buf, 0, "%s", "123456789012345");
        Check(n == 15 && strlen(g.buf) == 15 && g.Whole(), "a text that fills the buffer exactly");
    }
    {  // cut, then more appends: the length stays at cap - 1 and nothing is written behind the buffer
        Guarded g;
        int n = AppendF(g.buf, sizeof g.buf, 0, "%s", "0123456789");
        n = AppendF(g.buf, sizeof g.buf, n, "%s", "abcdefghijklmnopqrstuvwxyz");
        Check(n == 15 && !strcmp(g.buf, "0123456789abcde") && g.Whole(), "a cut append ends at the buffer's last byte");
        for (int i = 0; i < 100; ++i) n = AppendF(g.buf, sizeof g.buf, n, " %08X", 0xDEADBEEFu);
        g.buf[n] = 0;  // the closing terminator many callers write
        Check(n == 15 && !strcmp(g.buf, "0123456789abcde") && g.Whole(), "100 appends to a full buffer change nothing");
    }
    {  // lengths from elsewhere: negative, and past the end
        Guarded g;
        int n = AppendF(g.buf, sizeof g.buf, -5, "%s", "neg");
        Check(n == 3 && !strcmp(g.buf, "neg") && g.Whole(), "a negative length appends at the start");
        memset(g.buf, 'x', sizeof g.buf);
        n = AppendF(g.buf, sizeof g.buf, 4000, "%s", "far");
        Check(n == 15 && g.buf[15] == 0 && g.Whole(), "a length past the end is clamped and the buffer terminated");
    }
    {  // tiny buffers
        Guarded g;
        Check(AppendF(g.buf, 0, 0, "%s", "abc") == 0 && g.buf[0] == 'x', "capacity 0: nothing written");
        Check(AppendF(g.buf, 1, 0, "%s", "abc") == 0 && g.buf[0] == 0 && g.buf[1] == 'x', "capacity 1: only the terminator");
        Check(AppendF(g.buf, 2, 0, "%s", "abc") == 1 && g.buf[0] == 'a' && g.buf[1] == 0 && g.buf[2] == 'x', "capacity 2: one character");
    }
    {  // an empty append keeps the text and the terminator
        Guarded g;
        int n = AppendF(g.buf, sizeof g.buf, 0, "%s", "abc");
        n = AppendF(g.buf, sizeof g.buf, n, "%s", "");
        Check(n == 3 && !strcmp(g.buf, "abc"), "an empty append");
    }
    {  // what the old pattern did: after one cut line the next call is handed a size that wrapped around
        Guarded g;
        int n = 0;
        n += snprintf(g.buf + n, sizeof g.buf - n, "%s", "0123456789abcdefghij");  // wants 20, n is now 20 > 16
        const size_t next = sizeof g.buf - n;                                      // 16 - 20 as size_t
        Check(n == 20 && next > 0x7FFFFFFFu, "the old pattern's length passes the capacity and its next size wraps around");
        printf("old pattern after one cut line: n = %d of %u, next size argument %zu\n", n, (unsigned)sizeof g.buf, next);
    }
    printf("%d checks, %d failed\n", gChecks, gFailed);
    printf(gFailed ? "FAILED\n" : "ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
