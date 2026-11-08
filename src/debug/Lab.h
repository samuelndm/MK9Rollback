#pragma once

// Lab diagnostics: the restore audits, bone/pad captures, per-frame state hashes, address-space and type censuses, the
// RNG/VM trace patches, the rigid-body watch, the frames log's object lines (with stack captures) and the online
// connection write-watch. A player's game runs without them: 2026-10-31 the per-tick audits drew fights black, and a
// Safe-mode player's game carried 53 patches for 19 it needed (docs/restructure_plan.md, phase 1).
// 'lab 1' turns them on; send it before 'objhooks' / 'rbmode' / 'netcode', since the patch sets are chosen at install.
// Test scripts that read these diagnostics (soaks, trials, hash and A/B comparisons) send it first.
namespace Lab {
inline volatile bool gOn = false;
inline bool On() { return gOn; }
}  // namespace Lab
