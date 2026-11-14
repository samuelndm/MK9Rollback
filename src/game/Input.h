#pragma once
#include <cstdint>

// MK9's per-player gamepad object (PlayerInfo+0xB40). Hardware pads live in a
// 6-slot array at 0x00E221A8 (stride 0x60C); AI/remote pads elsewhere.
namespace Input {

struct PadSnapshot {
    bool valid;            // PlayerInfo had a readable pad pointer
    uint32_t pad;          // pad object address
    int32_t index;         // pad+0x04  pad slot (P1=0 hardware, Practice AI P2=15)
    uint32_t flags;        // pad+0x08  1 = hardware-sampled, 8 = externally driven (sampler only copies prev=cur)
    uint32_t cur[3];       // pad+0x0C  current record (cur[0] = button/direction mask)
    uint32_t prev[3];      // pad+0x18  previous record
    uint8_t ringIndex[3];  // pad+0x54..0x56 press / direction / release ring write indices
    uint32_t inputLock;    // PlayerInfo+0x944, 1 = command queries return 0
};

// The fighting-game input for one frame: MK9's processed mask (pad cur[0]).
// Directions are screen-absolute: 1 Up, 2 Down, 4 Left, 8 Right.
struct MKInput {
    uint32_t mask;
};

PadSnapshot ReadPad(int player);
MKInput ReadPlayerInput(int player);

// Drive a player's pad for the current frame exactly like MK9's own online
// remote-input path (0x00881360 + 0x0087F440): mark the pad externally driven
// (flags |= 8, so the hardware sampler leaves it alone), prev = cur,
// cur[0] = mask, then let the engine derive press/release ring events.
// Must be called on the game thread after the sampler and before the fight
// update, i.e. at EngineTick entry.
bool OverridePlayerInput(int player, const MKInput& input);

// GGPO sessions: a fighter pad must get exactly one OverridePlayerInput per frame, live and resimulated alike
// (a replay has no sampler and applies once; a live frame that also released the pad to the sampler, or applied
// a scripted mask and then the synchronized one, rotated prev/cur and pushed ring events differently). Scripted
// input (CommandChannel) is only recorded as wanted; Rollback::RecordInputs applies the synchronized mask once.
void SetWantedInput(int player, uint32_t mask);
uint32_t TakeWantedInput(int player);  // 0 when nothing was wanted this frame

// Give the pad back to the hardware sampler (restores the original flag bit).
void ReleasePlayerInput(int player);
void ReleaseAll();
bool IsOverridden(int player);

// Raw hardware pad records 0..5 (0x00E221A8 + i*0x60C), for driving menus.
bool OverrideHwPad(int index, uint32_t mask);
void ReleaseHwPad(int index);
bool HwPadOverridden(int index);

// Netplay: the local player's real input. The hardware sampler's call is wrapped so pads we drive are still sampled
// (and then restored); LocalHardwareMask is the OR of every hardware pad's sampled mask this iteration.
bool InstallSamplerHook();
void SetLocalCapture(bool on);
uint32_t LocalHardwareMask();
int64_t LocalSampleQpc();  // when that mask was sampled (QueryPerformanceCounter ticks; 0 = no capture)
// Late sampling (rc21): while on, the main loop's sampler call is deferred to SampleDeferredNow (the P2P boundary's
// start, after MK9's frame-limiter wait), one sample per main-loop pass as before.
void SetDeferSampling(bool on);
void SampleDeferredNow();
void LogSamplingCounts();

// A panel of the mod has the controls (the stage screen, net\StageFlow.cpp). While on:
//  - every engine tick the six hardware pad records are driven with a neutral mask (HoldPadsNeutral, from
//    CommandChannel::RunFrame, after the sampler and before the game reads them), so the game's screen underneath sees
//    no direction and no button, and MK9's online exchange sends neutral input for this player;
//  - local capture is on, so the sampler still reads the real devices and puts their bytes back: PanelMask is what the
//    player holds (every hardware pad OR-ed, keyboard pads included), plus a scripted 'hwhold' press of the test
//    harness, which goes to the panel instead of the game while the panel has the controls.
// Covers what MK9 reads through the pad records. Whether a screen of the game also takes keys another way (the queued
// key events at 0x00E28EF0) is not known from the disassembly; docs\feature_parity_plan.md 4.2 says how it was tested.
void SetPanelHold(bool on);
bool PanelHold();
void HoldPadsNeutral();
void SetPanelScripted(uint32_t mask);
uint32_t PanelMask();
// A press the panel makes on the game's screen in the player's place (the choice after a match answers MK9's own rematch
// box, Refight.h): while the panel has the controls, every hardware pad reads this mask instead of "nothing held", from
// the next tick's HoldPadsNeutral on. 0 again: nothing held. Forgotten when the panel gives the controls back.
void SetPanelPass(uint32_t mask);
// What the panel's reader adds to PanelMask once per tick: every bit a sample showed since its last call (a pass of the
// main loop may sample without a tick), and what the pads showed between two samples (PressLatch.h). A key pressed and
// released between two samples is in there too, because its release was held back until one sample had it.
uint32_t TakePanelPresses();

// Netplay start: both fighters' input history (pad records past the header, their rings, the last-buttons lists)
// cleared to the same neutral state on both peers. Returns the number of pads reset.
int CanonicalizePadsForNetplay();
}  // namespace Input
