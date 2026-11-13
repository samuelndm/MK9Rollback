#pragma once

// A key's presses, from polls of GetAsyncKeyState (Overlay.cpp's watcher thread; tests\host\keyedge_test.cpp).
// Bit 15 of a poll's result: the key is down now. Bit 0: it went down since the previous GetAsyncKeyState call for it,
// which catches a press that began and ended between two polls, or during a poll that had to wait. Bit 0 alone is not
// taken on the first poll after a pause (Reset), where it may be minutes old, nor on the poll after the key was seen
// down, where it is that same press or its auto-repeat.
struct KeyEdge {
    bool prev = false, fresh = true;
    void Reset() {
        prev = false;
        fresh = true;
    }
    bool Poll(short state) {  // true: one press to act on
        const bool down = (state & 0x8000) != 0;
        const bool press = down ? !prev : (!prev && !fresh && (state & 1) != 0);
        prev = down;
        fresh = false;
        return press;
    }
};
