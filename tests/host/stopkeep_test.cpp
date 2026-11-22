// src\rollback\StopKeep.h: which held blocks a mid-fight session stop keeps, the block hash of the STALEWRITE check
// (a write anywhere in the first 1 KB changes it; unreadable memory is reported, not hashed), its checkpoints, and the
// names of MK9's disconnect types and callers.
#include <cstdio>
#include <cstring>
#include <vector>

#include "StopKeep.h"

namespace {
int gChecks = 0, gFailed = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s\n", what);
}

// A fake address space: 4 KB at 0x10000; anything else is unreadable.
std::vector<unsigned char> gMem(4096);
constexpr uint32_t kBase = 0x10000;
bool Read(uint32_t a, uint32_t* v) {
    if (a < kBase || a + 4 > kBase + gMem.size()) return false;
    memcpy(v, &gMem[a - kBase], 4);
    return true;
}
uint32_t Hash(uint32_t addr, uint32_t size) {
    uint32_t h = 0;
    return StopKeep::HashBlock(addr, size, Read, &h) ? h : 0;
}
}  // namespace

int main() {
    Check(StopKeep::KeepKind(0) && StopKeep::KeepKind(2) && !StopKeep::KeepKind(1),
          "GMalloc and small-block blocks are kept, MK heap blocks given back");

    for (size_t i = 0; i < gMem.size(); ++i) gMem[i] = (unsigned char)(i * 7 + 3);
    const uint32_t blk = kBase + 0x100;
    const uint32_t h704 = Hash(blk, 704);
    Check(h704 != 0, "a readable 704-byte block hashes");
    Check(Hash(blk, 704) == h704, "the same bytes give the same hash");
    bool every = true;
    for (uint32_t o = 0; o < 704; ++o) {
        gMem[blk - kBase + o] ^= 0x40;
        if (Hash(blk, 704) == h704) every = false;
        gMem[blk - kBase + o] ^= 0x40;
    }
    Check(every, "a one-byte write anywhere in a 704-byte block changes its hash");
    Check(Hash(blk, 704) == h704, "the block is as before after the probes");
    {  // "Emit" over the free-list link at offset 0: the 2026-11-20 crash's pointer 0x74696D45
        unsigned char save[4];
        memcpy(save, &gMem[blk - kBase], 4);
        memcpy(&gMem[blk - kBase], "Emit", 4);
        Check(Hash(blk, 704) != h704, "text written over the first dword is seen");
        memcpy(&gMem[blk - kBase], save, 4);
    }
    const uint32_t h2k = Hash(kBase, 2048);
    gMem[1500] ^= 1;
    Check(Hash(kBase, 2048) == h2k, "bytes past the first 1 KB are not hashed (bounded time)");
    gMem[1500] ^= 1;
    gMem[1000] ^= 1;
    Check(Hash(kBase, 2048) != h2k, "a byte inside the first 1 KB is");
    gMem[1000] ^= 1;
    const uint32_t h0 = Hash(blk, 0);
    gMem[blk - kBase + 12] ^= 1;
    Check(Hash(blk, 0) != h0, "a block of unknown size: its first 16 bytes are hashed");
    gMem[blk - kBase + 12] ^= 1;
    gMem[blk - kBase + 20] ^= 1;
    Check(Hash(blk, 0) == h0, "and nothing past them");
    gMem[blk - kBase + 20] ^= 1;
    uint32_t out = 0xABCD;
    Check(!StopKeep::HashBlock(kBase + 4096 - 8, 704, Read, &out) && out == 0xABCD,
          "a block that runs into unreadable memory is reported unreadable, the hash left alone");
    Check(!StopKeep::HashBlock(0x500, 16, Read, &out), "an unreadable block is reported");
    Check(StopKeep::HashBlock(blk, 3, Read, &out), "a size under one dword hashes nothing and does not read past it");

    {
        StopKeep::Schedule s;
        Check(s.Due(0) == 0 && s.Due(1999) == 0, "no checkpoint before 2 s");
        Check(s.Due(2000) == 2000 && s.Due(2500) == 0, "2 s once");
        Check(s.Due(9000) == 0 && s.Due(10000) == 10000, "then 10 s");
        Check(!s.Done(), "not done after two");
        Check(s.Due(95000) == 90000 && s.Due(95000) == 0, "a late call takes the passed ones in one line (the latest)");
        Check(s.Done(), "done after 90 s");
        StopKeep::Schedule t;
        Check(t.Due(31000) == 30000 && t.Due(40000) == 0 && t.Due(90000) == 90000 && t.Done(),
              "a game paused past several checkpoints: one line, then the rest in order");
    }

    Check(!strcmp(StopKeep::DisconnectTypeName(0), "DT_None"), "type 0");
    Check(!strcmp(StopKeep::DisconnectTypeName(1), "DT_LostMatchConnection"), "type 1");
    Check(!strcmp(StopKeep::DisconnectTypeName(2), "DT_MatchConnectionDesync"), "type 2");
    Check(!strcmp(StopKeep::DisconnectTypeName(6), "DT_MatchQuit"), "type 6");
    Check(!strcmp(StopKeep::DisconnectTypeName(10), "DT_LostLinkConnection"), "type 10");
    Check(!strcmp(StopKeep::DisconnectTypeName(11), "DT_LostMatchConnectionForced"), "type 11");
    Check(!strcmp(StopKeep::DisconnectTypeName(12), "INVALID_DISCONNECT_TYPE") &&
              !strcmp(StopKeep::DisconnectTypeName(0xFFFFFFFF), "INVALID_DISCONNECT_TYPE"),
          "anything else is invalid, never out of the table");
    Check(!strcmp(StopKeep::DisconnectCallerName(0x0081C679), "MK9's Disconnect(type) 0x0081C640"), "the wrapper");
    Check(!strcmp(StopKeep::DisconnectCallerName(0x0094501C), "the link watch 0x00944FC0") &&
              !strcmp(StopKeep::DisconnectCallerName(0x00945048), "the link watch 0x00944FC0"),
          "the link watch's two calls");
    Check(!strcmp(StopKeep::DisconnectCallerName(0x12345678), "another caller"), "an unknown return");

    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (gFailed) return 1;
    printf("ALL CHECKS PASSED\n");
    return 0;
}
