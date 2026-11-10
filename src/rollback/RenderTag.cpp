#include "RenderTag.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "RollbackManager.h"

extern "C" {
extern uint32_t MK9R_BeginVtables[2];  // [0] a live frame's command, [1] a replayed frame's
extern uint32_t MK9R_BeginQueued;      // commands queued through the stub (game thread)
}

namespace {
constexpr uintptr_t kVtable = 0x00D59D08;  // begin-drawing render command: {destructor, Execute, DescribeCommand}
constexpr uintptr_t kExecute = 0x00753620;
constexpr uintptr_t kEnqueueSites[2] = {0x00756C0E, 0x00757A47};
constexpr uint8_t kEnqueueBytes[6] = {0xC7, 0x00, 0x08, 0x9D, 0xD5, 0x00};  // mov dword [eax], 0x00D59D08
constexpr uintptr_t kLateRead = 0x00666DF9;
constexpr uint8_t kLateReadBytes[5] = {0xA0, 0xA5, 0x44, 0xE8, 0x00};  // mov al, [0x00E844A5]
constexpr uintptr_t kSkipFlag = 0x00E84705;  // set by RHIBeginDrawingViewport: this frame's draws and present are left out
constexpr LONG kMaxRun = 16;                 // replayed frames in a row before one is drawn anyway (a replay is at most ~10)
constexpr uint32_t kStampSlots = 64;         // queued and not yet executed commands are a handful at most

uint32_t gLiveVtable[3], gReplayVtable[3];
bool gInstalled = false;
volatile LONG gSkipWanted = 1;

std::atomic<int64_t> gStamp[kStampSlots];  // by queue count; written by the game thread, taken by the render thread

// Render thread.
uint32_t gExecuted = 0;  // commands executed through the wrappers: the same count as MK9R_BeginQueued, later
int64_t gDrawnStamp = 0;
LONG gRun = 0;  // replayed frames skipped since the last drawn frame
volatile LONG gSkipped = 0, gDrawn = 0, gForced = 0;
LONG gForcedLogged = 0;

using ExecuteFn = uint32_t(__thiscall*)(void*);

int64_t NextStamp() { return gStamp[gExecuted++ % kStampSlots].exchange(0, std::memory_order_relaxed); }

bool PatchCall(uintptr_t at, const uint8_t* expected, uint32_t length, void (*stub)(), const char* name) {
    uint8_t code[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(stub) - (at + 5));
    memcpy(code + 1, &rel, 4);
    return CodePatch::Write(at, expected, code, length, name);
}
}  // namespace

extern "C" {
uint32_t MK9R_BeginVtables[2] = {kVtable, kVtable};  // the game's own vtable until Install
uint32_t MK9R_BeginQueued = 0;
const bool* MK9R_ReplayFlag = &Rollback::gIsResimulating;
uint8_t MK9R_SkipThisDraw = 0;  // render thread: set around the Execute of a replayed frame's command
void MK9R_BeginTagStub();
void MK9R_LateReadStub();

// Execute of the two vtable copies (fastcall with a dummy EDX = thiscall without stack arguments).
uint32_t __fastcall MK9R_BeginDrawLive(void* self, void* /*edx*/) {
    gDrawnStamp = NextStamp();
    gRun = 0;
    return reinterpret_cast<ExecuteFn>(kExecute)(self);
}
uint32_t __fastcall MK9R_BeginDrawReplayed(void* self, void* /*edx*/) {
    NextStamp();  // a replayed frame has none; its slot is passed over
    gDrawnStamp = 0;
    if (!gSkipWanted) return reinterpret_cast<ExecuteFn>(kExecute)(self);
    if (gRun >= kMaxRun) {  // the replay flag never dropped: show something
        gRun = 0;
        InterlockedIncrement(&gForced);
        return reinterpret_cast<ExecuteFn>(kExecute)(self);
    }
    MK9R_SkipThisDraw = 1;
    const uint32_t size = reinterpret_cast<ExecuteFn>(kExecute)(self);
    MK9R_SkipThisDraw = 0;
    if (*reinterpret_cast<const uint8_t*>(kSkipFlag)) {
        ++gRun;
        InterlockedIncrement(&gSkipped);
    } else {
        InterlockedIncrement(&gDrawn);  // one of the game's own conditions said draw
    }
    return size;
}
}

// _MK9R_BeginTagStub replaces "mov dword [eax], 0x00D59D08" (game thread): the same store with the vtable for this
// kind of frame, and the queue count; every register and the flags are kept (lea, not inc).
// _MK9R_LateReadStub replaces "mov al, [0x00E844A5]" (render thread): the limiter's late flag, or this frame's mark.
// The instruction after it is "test al, al", so the flags are free.
asm(R"(
    .text
    .globl _MK9R_BeginTagStub
_MK9R_BeginTagStub:
    pushl %ecx
    movl _MK9R_ReplayFlag, %ecx
    movzbl (%ecx), %ecx
    movl _MK9R_BeginVtables(,%ecx,4), %ecx
    movl %ecx, (%eax)
    movl _MK9R_BeginQueued, %ecx
    leal 1(%ecx), %ecx
    movl %ecx, _MK9R_BeginQueued
    popl %ecx
    ret
    .globl _MK9R_LateReadStub
_MK9R_LateReadStub:
    movb 0x00E844A5, %al
    orb _MK9R_SkipThisDraw, %al
    ret
)");

namespace RenderTag {

bool Install() {
    if (gInstalled) return true;
    const uint32_t* vt = reinterpret_cast<const uint32_t*>(kVtable);
    if (vt[1] != kExecute) {
        Logger::Info("RENDERTAG: render command vtable 0x00D59D08 not as expected; replayed frames are drawn, no LATENCY lines");
        return false;
    }
    memcpy(gLiveVtable, vt, sizeof gLiveVtable);
    memcpy(gReplayVtable, vt, sizeof gReplayVtable);
    gLiveVtable[1] = (uint32_t)reinterpret_cast<uintptr_t>(&MK9R_BeginDrawLive);
    gReplayVtable[1] = (uint32_t)reinterpret_cast<uintptr_t>(&MK9R_BeginDrawReplayed);
    // The vtables before the sites: every command the stub counts must run a wrapper, which counts it again on the
    // render thread (the stamps are found by that count). The late-flag read last: without it a replayed frame's mark
    // is simply not seen and the frame is drawn.
    MK9R_BeginVtables[0] = (uint32_t)reinterpret_cast<uintptr_t>(gLiveVtable);
    MK9R_BeginVtables[1] = (uint32_t)reinterpret_cast<uintptr_t>(gReplayVtable);
    int sites = 0;
    for (const uintptr_t at : kEnqueueSites) {
        if (PatchCall(at, kEnqueueBytes, 6, &MK9R_BeginTagStub, "begin-drawing command vtable store (rendertag)")) ++sites;
        else Logger::Info("RENDERTAG: %08X not as expected; frames queued there are drawn and carry no stamp", (unsigned)at);
    }
    if (!sites) return false;
    gInstalled = true;
    if (!PatchCall(kLateRead, kLateReadBytes, 5, &MK9R_LateReadStub, "RHIBeginDrawingViewport late-flag read 0x00666DF9 (replayskip)"))
        Logger::Info("RENDERTAG: 0x00666DF9 not as expected; replayed frames are drawn");
    return true;
}

void SetReplaySkip(bool on) {
    InterlockedExchange(&gSkipWanted, on ? 1 : 0);
    Logger::Info("REPLAYSKIP: replayed frames are %s", on ? "not drawn" : "drawn and presented (rc27)");
}

void Disable() {
    MK9R_BeginVtables[0] = MK9R_BeginVtables[1] = (uint32_t)kVtable;
}

void SetStamp(int64_t qpc) {
    if (gInstalled) gStamp[MK9R_BeginQueued % kStampSlots].store(qpc, std::memory_order_relaxed);
}

int64_t TakeStamp() {
    const int64_t stamp = gDrawnStamp;
    gDrawnStamp = 0;
    return stamp;
}

void SessionReset() {
    InterlockedExchange(&gSkipped, 0);
    InterlockedExchange(&gDrawn, 0);
    InterlockedExchange(&gForced, 0);
}

void LogSession() {
    const LONG forced = gForced;
    Logger::Info("REPLAYSKIP this session: %ld replayed frames not drawn, %ld drawn (the game's own rule), %ld drawn after %ld "
                 "skipped in a row%s",
                 gSkipped, gDrawn, forced, kMaxRun, gInstalled && gSkipWanted ? "" : "; replayskip is off or not installed");
    if (forced && !InterlockedExchange(&gForcedLogged, 1))
        Logger::Info("REPLAYSKIP: more than %ld replayed frames in a row were queued: the replay flag stayed set (a bug; "
                     "the picture ran at a few frames a second meanwhile)", kMaxRun);
}

}  // namespace RenderTag
