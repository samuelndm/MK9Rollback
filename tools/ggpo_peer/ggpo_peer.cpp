// Stand-in remote player for MK9Rollback's GGPO peer-to-peer tests (Milestone 13, no second game needed).
// GGPO P2P exchanges only inputs, so a peer with a trivial game state is a real remote endpoint for the
// game: it sends random held inputs over UDP at 60 Hz, and the game predicts, receives late inputs and
// rolls back exactly as against a real opponent. (Cross-peer desync detection needs a second game.)
//   ggpo_peer <localport> <remote ip> <remote port> <player 1|2> <seconds> [delay frames]
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ggponet.h"

// The patched GGPO library reports SyncTest errors through this hook (unused in P2P).
extern "C" void MK9R_GgpoSyncError(const char* msg) { printf("sync error: %s\n", msg); }

namespace {
GGPOSession* gS = nullptr;
int gFrame = 0;
GGPOPlayerHandle gLocal = GGPO_INVALID_HANDLE;
bool gRunning = false;

bool __cdecl BeginGame(const char*) { return true; }
bool __cdecl SaveState(unsigned char** buf, int* len, int* checksum, int) {
    int* p = static_cast<int*>(malloc(sizeof(int)));
    *p = gFrame;
    *buf = reinterpret_cast<unsigned char*>(p);
    *len = sizeof(int);
    *checksum = gFrame;
    return true;
}
bool __cdecl LoadState(unsigned char* buf, int) {
    gFrame = *reinterpret_cast<int*>(buf);
    return true;
}
bool __cdecl LogState(char*, unsigned char*, int) { return true; }
void __cdecl FreeBuffer(void* b) { free(b); }
bool __cdecl AdvanceFrame(int) {
    unsigned int in[2] = {0, 0};
    int disc = 0;
    ggpo_synchronize_input(gS, in, sizeof in, &disc);
    ++gFrame;
    ggpo_advance_frame(gS);
    return true;
}
bool __cdecl OnEvent(GGPOEvent* e) {
    switch (e->code) {
        case GGPO_EVENTCODE_CONNECTED_TO_PEER: printf("connected to peer\n"); break;
        case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER: printf("synchronized with peer\n"); break;
        case GGPO_EVENTCODE_RUNNING: printf("running\n"); gRunning = true; break;
        case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER: printf("disconnected from peer\n"); break;
        case GGPO_EVENTCODE_TIMESYNC: Sleep(1000 * e->u.timesync.frames_ahead / 60); break;
        default: break;
    }
    fflush(stdout);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("usage: ggpo_peer <localport> <remote ip> <remote port> <player 1|2> <seconds> [delay]\n");
        return 2;
    }
    const unsigned short localPort = (unsigned short)atoi(argv[1]);
    const int remotePort = atoi(argv[3]), me = atoi(argv[4]), seconds = atoi(argv[5]);
    const int delay = argc > 6 ? atoi(argv[6]) : 0;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    GGPOSessionCallbacks cb{};
    cb.begin_game = BeginGame;
    cb.save_game_state = SaveState;
    cb.load_game_state = LoadState;
    cb.log_game_state = LogState;
    cb.free_buffer = FreeBuffer;
    cb.advance_frame = AdvanceFrame;
    cb.on_event = OnEvent;
    char name[] = "MK9Rollback";
    if (ggpo_start_session(&gS, &cb, name, 2, sizeof(unsigned int), localPort) != GGPO_OK) {
        printf("start_session failed\n");
        return 1;
    }
    ggpo_set_disconnect_timeout(gS, 5000);
    ggpo_set_disconnect_notify_start(gS, 1000);
    for (int p = 1; p <= 2; ++p) {
        GGPOPlayer pl{};
        pl.size = sizeof pl;
        pl.player_num = p;
        GGPOPlayerHandle h;
        if (p == me) {
            pl.type = GGPO_PLAYERTYPE_LOCAL;
            ggpo_add_player(gS, &pl, &h);
            gLocal = h;
            if (delay) ggpo_set_frame_delay(gS, h, delay);
        } else {
            pl.type = GGPO_PLAYERTYPE_REMOTE;
            strncpy(pl.u.remote.ip_address, argv[2], sizeof pl.u.remote.ip_address - 1);
            pl.u.remote.port = (unsigned short)remotePort;
            ggpo_add_player(gS, &pl, &h);
        }
    }
    // Checksum side channel (game: GGPO port + 100): this peer has no game state, so it echoes the game's
    // packets back. The game then sees every frame "match", which tests the channel itself.
    SOCKET cs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in csLocal{};
    csLocal.sin_family = AF_INET;
    csLocal.sin_port = htons((unsigned short)(localPort + 100));
    u_long nb = 1;
    if (cs == INVALID_SOCKET || bind(cs, reinterpret_cast<sockaddr*>(&csLocal), sizeof csLocal) || ioctlsocket(cs, FIONBIO, &nb)) {
        if (cs != INVALID_SOCKET) closesocket(cs);
        cs = INVALID_SOCKET;
    }
    int echoed = 0;
    srand(12345);
    unsigned int mask = 0;
    int hold = 0;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    double next = 0;
    int lastReport = 0;
    const unsigned int dirs[] = {0, 0, 4, 8, 4, 8, 2, 1}, atk[] = {0, 0, 0, 0x10, 0x20, 0x40, 0x80, 0x8000};
    for (;;) {
        QueryPerformanceCounter(&t);
        const double now = double(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now > seconds) break;
        ggpo_idle(gS, 1);
        for (int n = 0; cs != INVALID_SOCKET && n < 16; ++n) {
            char buf[64];
            sockaddr_in from{};
            int fromLen = sizeof from;
            const int got = recvfrom(cs, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (got <= 0) break;
            sendto(cs, buf, got, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
            ++echoed;
        }
        if (now < next) {
            Sleep(1);
            continue;
        }
        next += 1.0 / 60.0;
        if (--hold <= 0) {
            mask = dirs[rand() % 8] | atk[rand() % 8];
            hold = 4 + rand() % 20;
        }
        if (gRunning && GGPO_SUCCEEDED(ggpo_add_local_input(gS, gLocal, &mask, sizeof mask))) {
            unsigned int in[2];
            int disc = 0;
            if (GGPO_SUCCEEDED(ggpo_synchronize_input(gS, in, sizeof in, &disc))) {
                ++gFrame;
                ggpo_advance_frame(gS);
            }
        }
        if ((int)now != lastReport) {
            lastReport = (int)now;
            GGPONetworkStats st{};
            for (int h = 0; h < 2; ++h)
                if (h != gLocal && GGPO_SUCCEEDED(ggpo_get_network_stats(gS, h, &st)))
                    printf("t=%ds frame=%d ping=%dms behind local=%d remote=%d kbps=%d\n", lastReport, gFrame, st.network.ping,
                           st.timesync.local_frames_behind, st.timesync.remote_frames_behind, st.network.kbps_sent);
            fflush(stdout);
        }
    }
    ggpo_close_session(gS);
    if (cs != INVALID_SOCKET) closesocket(cs);
    printf("done: %d frames, %d checksum packets echoed\n", gFrame, echoed);
    return 0;
}
