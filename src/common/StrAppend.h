#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdio>

// Appends formatted text to buf (capacity cap) at offset n and returns the new length, never more than cap - 1: text
// that does not fit is cut at the buffer's end, and the buffer stays terminated.
//
//     n = AppendF(line, sizeof line, n, " %08lX", v);
//
// It replaces `n += snprintf(line + n, sizeof line - n, ...)`, which goes wrong once a line is cut: snprintf returns the
// length it wanted to write, n passes the capacity, the next size argument wraps to a huge unsigned number and the next
// call writes past the buffer (as does a closing `line[n] = 0`). The hang report's module list (900 bytes for up to 40
// "name.dll+offset" entries) and the crash report's (700 bytes for 16 entries of up to 160) could reach that.
inline int AppendF(char* buf, size_t cap, int n, const char* fmt, ...) {
    if (!cap) return 0;
    if (n < 0) n = 0;
    if ((size_t)n >= cap - 1) {  // full already (or a length from elsewhere that is past the end)
        buf[cap - 1] = 0;
        return (int)(cap - 1);
    }
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    if (w < 0) {  // a format the C runtime refuses: nothing appended
        buf[n] = 0;
        return n;
    }
    return (size_t)w >= cap - (size_t)n ? (int)(cap - 1) : n + w;
}
