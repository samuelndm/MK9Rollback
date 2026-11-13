#pragma once

// What the PC did around a stall (docs\todo.md, "Stalls of 150-240 ms near a match's end"). The picture of a fullscreen
// game stops for as long as another program keeps Windows' window manager busy, and on the player's PC Windows noted
// a power-status change at the start of each of the five stalls of one evening without saying what changed. This
// writes down, as SYSEVENT lines with the log's time stamps:
//   - what Windows announces to every program: power status and power settings (mains or battery, battery percent,
//     battery saver, display state, power plan, lid), the effective power mode (game mode among them), display mode,
//     device and setting changes, the theme, the clock, low memory, a registered broadcast's name;
//   - which program comes to the front, the Alt+Tab switcher, a switch of the desktop (lock screen, elevation prompt);
//   - each window of another program that appears: the program's file name, the window's class and its size;
//   - after a long wait for the window manager (KEYWAIT), the programs that used the most processor time around it.
// Only program file names and window class names are written, never a window's title, a path or a user name.
//
// It listens and never asks: one hidden window that is never shown (broadcasts reach only top-level windows), power
// setting registrations, two out-of-context event hooks. All of it lives on a thread of its own (the listener), which
// sleeps until Windows has something for it, so the game thread and the render thread never run a line of it.
// 'syswatch 0' turns it off.
namespace SysWatch {
using Sink = void (*)(const char* line);  // takes one finished log line; called by the listener and by a system thread

// Start, Stop, Sample and Busiest belong to one thread (the overlay's watcher).
bool Start(Sink sink);  // starts the listener and waits until it is set up (false: nothing could be set up)
void Stop();            // ends the listener, which takes everything down again: after it the module can be unloaded

// Processor time by program. Sample takes the reference reading (the watcher: about once a second while a session
// runs, a millisecond or two). Busiest takes another and writes the programs that used the most in between, largest
// first ("name total ms (kernel ms)"); it returns the ms between the two readings, or -1 without a reference.
void Sample();
int Busiest(char* out, int n);

void* Window();  // the hidden window, null while stopped (tests; it belongs to the listener thread)
}  // namespace SysWatch
