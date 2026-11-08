#pragma once
#include <cstdint>

// MK object lifetime instrumentation (Gate D churn study).
//   0x0042E000 MKBaseObj ctor  (thiscall, plain ret, returns this): entry -> naked stub -> trampoline
//   0x0042D630 MKBaseObj dtor  (thiscall leaf: 3 stores, ret):     entry -> naked stub re-executing them
// Both stubs preserve every register and EFLAGS, because the game's callers
// may rely on registers the leaf does not touch (VS2008 /GL custom conventions).
namespace ObjectHooks {

// Where the game thread is in the frame (set by GameLoopHook): 0 = between frames,
// 1 = world tick (vt+0xBC), 2 = engine tick (vt+0xC0); events from other threads = 10 + game-thread phase.
extern volatile uint8_t gPhase;
extern volatile uint32_t gGameThread;

bool Install();
void Uninstall();
long InFlight();

// Game thread, end of EngineTick: resolve and log objects created/destroyed this tick.
void FlushFrame(uint32_t gameFrame);

// Object births/deaths since a mark (used by rollback guards). Mark() also
// starts tracking which objects are born after it.
void Mark();
// Load step 1: destroy (via their own deleting destructor, vt[1](obj,1)) every
// actor-owning MKObj born since the mark and still alive. Returns the count.
int DestroyBornSinceMark();
bool Installed();

// Live objects created while the hooks were installed (type = descriptor chain).
void ForEachLive(void (*fn)(uint32_t obj, const char* type, void* ctx), void* ctx);
// Every live MK object via the game's global object list (works without the hooks).
void ForEachListed(void (*fn)(uint32_t obj, const char* type, void* ctx), void* ctx);
const char* TypeOf(uint32_t obj);  // descriptor chain "Type<Parent<...", or "?"

// Load step 0: false if an actor-owning object alive at the mark has died since
// and its type is not on the verified retire-safe list.
bool CanLoad(char* why, size_t whyLen);
// Load step 3 (after the heap restore): objects that were alive at the mark and
// died since are alive again in memory but their UE3 actors are gone. For the
// verified effect types, null obj+8 and run the deleting destructor.
int RetireResurrected();

// General form for multi-snapshot rollback (frame S = snapshot's GameFrame).
void StartTracking(uint32_t gf);
void RestartTracking(uint32_t gf);  // GameFrame was reset (new fight/round): history starts over
void TrimHistory(uint32_t oldestNeededGf);
bool CanLoadTo(uint32_t S, char* why, size_t whyLen);
int PreRestore(uint32_t S);
int PostRestore(uint32_t S);
uint32_t CreatedSinceMark();
uint32_t DestroyedSinceMark();

// Frame an MK object was created at (game thread). 0 if unknown (created before tracking).
uint32_t BirthGf(uint32_t obj);
}  // namespace ObjectHooks

