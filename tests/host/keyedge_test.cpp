// src\debug\KeyEdge.h: one press of F9 switches the display once, however the 25 ms polls fall on it.
// A state is what GetAsyncKeyState returned at a poll: 0x8000 down now, 0x0001 went down since the previous poll.
#include <cstdio>

#include "KeyEdge.h"

namespace {
int gChecks = 0, gFailed = 0;

constexpr short kUp = 0, kDown = (short)0x8000, kDownNew = (short)0x8001, kWasDown = 1;

// The number of presses a sequence of polls gives; -1 in the sequence is a pause (the display was off).
int Presses(const short* s, int n) {
    KeyEdge k;
    int presses = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] == -1) k.Reset();
        else if (k.Poll(s[i])) ++presses;
    }
    return presses;
}

template <int N>
void Expect(const char* what, const short (&s)[N], int want) {
    ++gChecks;
    const int got = Presses(s, N);
    if (got == want) return;
    ++gFailed;
    printf("FAIL: %s: %d press(es), wanted %d\n", what, got, want);
}
}  // namespace

int main() {
    Expect("key never touched", {kUp, kUp, kUp, kUp}, 0);
    Expect("one press over three polls", {kUp, kDownNew, kDown, kDown, kUp, kUp}, 1);
    Expect("held half a second with auto-repeat, released right after a repeat", {kUp, kDownNew, kDown, kDownNew, kDownNew, kWasDown, kUp}, 1);
    Expect("a press shorter than one poll interval", {kUp, kWasDown, kUp}, 1);
    Expect("a press made while a poll waited 200 ms", {kUp, kUp, kWasDown, kUp, kUp}, 1);
    Expect("two presses", {kUp, kDownNew, kUp, kDownNew, kUp}, 2);
    Expect("a press, then a short one between polls", {kUp, kDownNew, kUp, kWasDown, kUp}, 2);
    Expect("down at the first poll", {kDown, kDown, kUp}, 1);
    Expect("an old press at the first poll (pressed before the game looked)", {kWasDown, kUp, kUp}, 0);
    Expect("an old press after a pause", {kUp, -1, kWasDown, kUp}, 0);
    Expect("a press after a pause", {kUp, -1, kUp, kDownNew, kUp}, 1);
    Expect("held across a pause: the press that follows the pause counts again", {kUp, kDownNew, -1, kDown, kUp}, 2);
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (gFailed) return 1;
    printf("ALL CHECKS PASSED\n");
    return 0;
}
