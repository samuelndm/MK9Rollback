#pragma once
#include <cstdint>
#include <utility>
#include <vector>

// Rollback of MK9's in-fight cinematics (MwyCinema: X-rays, throws, effect sequences).
//
// An X-ray starts on one frame: SeqAct_Cinema (a long-lived Kismet action of the fighter's data) creates its group
// and track instances (CinemaGroupInst*, CinemaTrackInst*, InterpGroupInst; ~50 UObjects, outer = the action), the
// templates (CinemaTrack*) and MKCinemaSequence change, and both fighters get a cinematic motion controller and
// Physics 7 (RollbackManager MOTIONCTL). Each frame after it advances the instances; at the end the spawned-actor
// groups destroy what they spawned (0x004BB820 -> DestroyActor at 0x004BB852) and the instances become garbage.
// No snapshot span covered any of it, so a load across the start replayed against a cinematic already running (no
// instance born in the replay, GGPO SyncTest errors from the X-ray's frame on, xray_probe 2026-11-02).
//
// Here: every live UObject whose class name contains "Cinema" (or InterpGroupInst/InterpTrackInst; no actors, no
// class defaults) is saved per frame from +0x3C (UObject header excluded) to its PropertiesSize, raw: what it points
// at was alive at S and is kept alive through the window (instances and their TArray blocks are registered with the
// free quarantine, the end's destroys are deferred). Runtime classes (instances, SeqAct_Cinema, MKCinemaSequence)
// also get their TArray contents saved (ArrayProperty offsets from reflection). At a load:
//   - objects born after S (the abandoned timeline's instances) are left unreferenced for the garbage collector,
//     after the actors their spawned-actor groups own are destroyed;
//   - actors spawned after S by groups that existed at S are destroyed too;
//   - every saved object gets its bytes and arrays back (all or nothing: one garbage-collected = none);
//   - deferred end-of-cinematic destroys at gf >= S: the actor is given back if its group owns it again, else
//     destroyed.
namespace CinemaRestore {

extern bool gRestore;
extern bool gLevelVis;  // level visibility records' +0x5C bit 0 put back at loads ('abtest levelvis')
extern bool gLevelReady;  // replayed frames get the first pass's level-ready answers ('abtest levelready')
extern bool gKismet;      // Kismet sequence containers restored with the cinematic objects ('abtest kismet')
extern bool gLevelUndo;   // levels added to the world after the load point are taken out again ('abtest levelundo')
extern bool gLevelRedo;   // the same when the level is next looked up (0x00888484), so the add path reruns ('abtest levelredo')
extern bool gPartial;     // a load across a collection restores all but the cinematics naming collected objects ('abtest cinepartial')
extern bool gDeadOk;      // object fields dangling at the save already do not block the restore ('cinemadeadok', 'abtest cinemadeadok')
extern bool gDeadRefs;    // dead references no longer block the restore: cleared instead ('cinemadeadrefs', 'abtest cinemadeadrefs')
// 'kismettrace N': for the next N frames, each save logs (frames log, "KTRACE") the tracked objects' dwords that changed
// since the same pass's previous save, the level visibility records and VM wait fields; O = first pass, R = replay.
void Trace(int frames);
void Prewarm();  // fight ticks without a snapshot: the object scan's classification ahead of the first save  // restore at loads ('cinema 0|1', 'abtest cinema'); saves and the deferral run either way

bool Install();  // patches the spawned-actor teardown's DestroyActor call (0x004BB852)
void SetActive(bool on);  // defer that teardown's destroys (switched with ActorDefer)
void SetFrame(uint32_t gf);
void SetFighting(bool fought);  // this tick ran the fight update (deferral only in fights; see FightTickDone)
// Save of frame gf (ring slot gf % 12, RollbackManager's kRing): appends the blocks the quarantine must keep.
void Save(uint32_t gf, std::vector<uint32_t>* blocks);
// Load to S, after the quarantine handed back the blocks freed after S. Returns the objects restored (-1: none saved).
int OnLoad(uint32_t S);
// The ranges the last OnLoad wrote back (object bytes, arrays), live; for FreeQuarantine::ProtectReferenced.
const std::vector<std::pair<uintptr_t, uint32_t>>& LastRestoredRanges();
// The saved objects the last OnLoad found garbage-collected since S ({object, size}): left out of the restore, so
// anything else the load put back that points into them is dangling (Snapshot::ScrubCompLists).
const std::vector<std::pair<uintptr_t, uint32_t>>& LastGoneRanges();
void Release(uint32_t oldestNeededGf);  // really destroy deferred actors whose teardown ran before oldestNeededGf
void ReleaseAll();
void DropAll();  // timeline restart: forget deferred actors (the level teardown owns them) and every save
uint32_t Pending();

}  // namespace CinemaRestore
