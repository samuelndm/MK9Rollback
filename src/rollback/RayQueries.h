#pragma once
#include <cstdint>

// MK object line checks: the MK object tick 0x008A7300 issues one per frame for a moving object through 0x006BDD50
// (call at 0x008A7564; receiver = the object's +0x1D8 helper, vtable 0x00C6A5B4) into the scene's batched PhysX query.
// 0x006BDC00 writes a provisional "done, no hit" (+8 = 1, +0xC = 0) inside the call; the query itself runs with the next
// physics step and its result arrives through the global report object 0x00DED22C (vtable 0x00D549E8: onBooleanQuery
// 0x006BDA80, onRaycastQuery 0x006BDB30, on the game thread or a PhysX worker), which set +8 / +0xC (and the hit data
// through the helper's vt[0]) during the next frame, before that frame's tick gate reads +0xC to spawn the object's
// pending script (0x0089F7F0).
//
// Queries pending at a frame boundary are in no snapshot. After a load, the ones issued in the abandoned future's last
// frame still answered into the restored helpers during the first replayed frame (a future ray's result), and the
// original's ones (issued in S-1, answered during S) were gone. Usually both say "no hit"; when a projectile is about to
// hit they differ: the tick gate reads +0xC 1 in the replay only (soak 12 13155 S=3254, 13109; "rec+C").
// Fix ('abtest rayqfix', default on): every issue counts as pending per helper until its result arrives; each save
// records the helpers with a pending query; a load marks every pending query stale (its result is dropped when it
// arrives during the load frame) and issues the saved frame's pending queries again, in the original's issue order,
// from proxies, copies of the restored helpers (start +0x34 -> end +0x40), whose results are forwarded to the helpers:
// told apart from the future's by userData, so the order PhysX reports them in does not matter.
// Soak 15 (BD3FC2F7, 6 sessions): 0 failing loads, 0 rec+C; in the first 600 loads of each, 130 re-issued results
// forwarded, none dropped, lost or without a slot.
// Not covered: 0x006F3E12 (0x006F3D50's embedded helpers, own query object, always asynchronous: the helper stays in
// flight until its result); dropping those would leave a restored helper waiting forever, so they need their issue
// arguments recorded and re-issued, not done (no failure traced to them: soak 14, 2,579 issues, 0 real failing loads).
//
// History: 2026-11-04 "not installed" because soak 13110 counted 0 queries left in flight (+8 = 0) after the issue: the
// provisional +8 = 1 hid them. Soak 14 (2026-11-06, the rayres ring with issue and gate entries) showed the order per
// frame: result (game or PhysX thread), gate read, issue "sync +C 0>0" — one result per issue, one frame later.
namespace RayQueries {
extern bool gFix;
bool Install();          // after RngTrace::Install (chains to its lab tap when present)
void OnSave(uint32_t gf);   // game thread, after a snapshot of frame gf was saved
void OnLoad(uint32_t S);    // game thread, after the snapshot restore and the quarantine's give-back
void Clear();            // a session started or stopped
void Report();           // counters (periodic)
}  // namespace RayQueries
