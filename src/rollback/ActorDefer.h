#pragma once
#include <cstdint>
#include <vector>

// Rollback-safe destruction of UE3 actors owned by MK objects.
//
// The MKObj destructor (0x004399F0) destroys the object's UE3 actor when the object owns it
// (obj+0x2C != 0): GWorld->DestroyActor(actor, 0, 1) at call site 0x00439A70 (callee
// 0x0079D960, thiscall, ret 0xC). Before that it may un-root the actor (0x005EDAB0 clears
// RF_RootSet 0x4000 in actor+8 when obj+0xC != 0), and after it clears the actor's back
// reference to the MK object (actor+0x9C {vt, obj +4, id +8}, reset by 0x0042D650).
//
// A load to a snapshot taken before such a death resurrects the MK object (heap image)
// pointing at its actor, so while rollback mode is on the DestroyActor call is deferred:
// the actor stays alive (for at most the snapshot window) and is destroyed once no loadable
// snapshot predates the death. A load to S un-defers deaths at gf >= S and puts the back
// reference and root flag back. This replaces refusing every rollback whose window
// contains the death of an effect object (about 35% of requests in the soaks).
namespace ActorDefer {

bool Install();
void SetActive(bool on);
void SetFrame(uint32_t gf);
// Really destroy actors whose MK owner died at gf < oldestNeededGf (game thread, boundary).
void Release(uint32_t oldestNeededGf);
void ReleaseAll();
// Timeline restart (fight/round boundary, level teardown): forget deferred actors without
// destroying them; the transition owns them (soak6 crash 0x0071C53C at gf=1).
void DropAll();
// Load to S: deaths at gf >= S are undone. Returns the number of actors given back.
int OnLoad(uint32_t S);
// True if the actor work of the MK object obj (with id) that died at gf >= S was deferred.
bool Deferred(uint32_t obj, uint32_t id);
uint32_t Pending();
// Destroy immediately (no deferral) while set: used when releasing deferred actors.
void SetBypass(bool on);
// While a rollback destroys the abandoned timeline's objects (born at/after S), only actors of
// those owners are destroyed for real. Objects that existed at S and die in the cascade (e.g.
// children re-parented to a post-snapshot object) are deferred like any in-window death and
// come back with OnLoad (run 2026-10-14 08:28: resurrected child pointing at a destroyed actor).
void SetBypassOwners(const std::vector<uint32_t>* owners);

}  // namespace ActorDefer
