// Test peer for the round 3 GGPO patches, adapted from tools\ggpo_peer\ggpo_peer.cpp (no game involved).
//   peer_test <localport> <remoteport> <player 1|2> <seconds> [bye=0|1] [timeout=ms] [delay=N] [norewind=0|1] [hashframes=N]
//             [churn=0|1]
//   peer_test bindtest <port>
// churn=1: the local input of frame n is Churn(n), about 16 of its 32 bits change every frame (ggpo_raw_peer's churncheck
// knows the function; use delay 0). The last line ends with the handshake's round trip (ggpo_sync_rtt_ms).
// Everything runs on 127.0.0.1 (ggpo_bind_loopback = 1: no firewall prompt). Times are wall-clock milliseconds
// (comparable between two processes). Build with -DOLDLIB against a library without the round 3 exports.
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ggponet.h"

extern "C" int ggpo_bind_loopback;
extern "C" int ggpo_no_disconnect_rewind;
extern "C" void (*ggpo_after_send)(void);  // counted here: one call per datagram the library sends
static int gAfterSend = 0;
static void CountSend() { ++gAfterSend; }
#ifndef OLDLIB
extern "C" int ggpo_bye_on_close;
extern "C" int ggpo_bind_failed;
extern "C" int ggpo_sync_rtt_ms;
#endif

static double NowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;  // 100 ns
    return (double)(t % 36000000000ULL) / 10000.0;  // ms within the hour
}

extern "C" void MK9R_GgpoSyncError(const char* msg) { printf("sync error: %s\n", msg); }
extern "C" void MK9R_GgpoAssert(const char* msg) {  // the library calls exit(0) right after
    printf("T=%.1f GGPO ASSERT: %s\n", NowMs(), msg);
    fflush(stdout);
}

namespace {
GGPOSession* gS = nullptr;
int gFrame = 0;
GGPOPlayerHandle gLocal = GGPO_INVALID_HANDLE;
bool gRunning = false, gDisconnected = false;
int gLoads = 0;
const int kRec = 100000;
unsigned int gRec[kRec][2];

void Record(const unsigned int* in) {
    if (gFrame >= 0 && gFrame < kRec) {
        gRec[gFrame][0] = in[0];
        gRec[gFrame][1] = in[1];
    }
}
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
    ++gLoads;
    return true;
}
bool __cdecl LogState(char*, unsigned char*, int) { return true; }
void __cdecl FreeBuffer(void* b) { free(b); }
bool __cdecl AdvanceFrame(int) {
    unsigned int in[2] = {0, 0};
    int disc = 0;
    ggpo_synchronize_input(gS, in, sizeof in, &disc);
    Record(in);
    ++gFrame;
    ggpo_advance_frame(gS);
    return true;
}
bool __cdecl OnEvent(GGPOEvent* e) {
    switch (e->code) {
        case GGPO_EVENTCODE_CONNECTED_TO_PEER: printf("T=%.1f connected to peer\n", NowMs()); break;
        case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER: printf("T=%.1f synchronized with peer\n", NowMs()); break;
        case GGPO_EVENTCODE_RUNNING: printf("T=%.1f running\n", NowMs()); gRunning = true; break;
        case GGPO_EVENTCODE_CONNECTION_INTERRUPTED: printf("T=%.1f connection interrupted (frame %d)\n", NowMs(), gFrame); break;
        case GGPO_EVENTCODE_CONNECTION_RESUMED: printf("T=%.1f connection resumed\n", NowMs()); break;
        case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
            printf("T=%.1f DISCONNECTED from peer (frame %d, loads so far %d)\n", NowMs(), gFrame, gLoads);
            gDisconnected = true;
            break;
        case GGPO_EVENTCODE_TIMESYNC: Sleep(1000 * e->u.timesync.frames_ahead / 60); break;
        default: break;
    }
    fflush(stdout);
    return true;
}
// churn=1: the same function as in ggpo_raw_peer.cpp.
unsigned int Churn(int frame) {
    unsigned int x = (unsigned int)frame * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    return x;
}
int Opt(int argc, char** argv, const char* key, int def) {
    const size_t n = strlen(key);
    for (int i = 5; i < argc; ++i)
        if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return atoi(argv[i] + n + 1);
    return def;
}
GGPOSessionCallbacks Callbacks() {
    GGPOSessionCallbacks cb{};
    cb.begin_game = BeginGame;
    cb.save_game_state = SaveState;
    cb.load_game_state = LoadState;
    cb.log_game_state = LogState;
    cb.free_buffer = FreeBuffer;
    cb.advance_frame = AdvanceFrame;
    cb.on_event = OnEvent;
    return cb;
}

#ifndef OLDLIB
// A session on a port that cannot be bound: ggpo_bind_failed is set, nothing asserts (add_player sends a sync request
// on the missing socket, the close sends its bye on it), and the next good bind resets it.
int BindTest(unsigned short port) {
    SOCKET blocker = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL on = TRUE;
    setsockopt(blocker, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(blocker, reinterpret_cast<sockaddr*>(&a), sizeof a)) {
        printf("bindtest: the blocker could not bind %u (%d)\n", port, WSAGetLastError());
        return 1;
    }
    GGPOSessionCallbacks cb = Callbacks();
    char name[] = "MK9Rollback";
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) closesocket(blocker);
        ggpo_bye_on_close = 1;
        const GGPOErrorCode rc = ggpo_start_session(&gS, &cb, name, 2, sizeof(unsigned int), port);
        printf("bindtest pass %d (%s): ggpo_start_session rc=%d, ggpo_bind_failed=%d\n", pass, pass ? "port free" : "port held exclusively",
               (int)rc, ggpo_bind_failed);
        for (int p = 1; p <= 2; ++p) {
            GGPOPlayer pl{};
            pl.size = sizeof pl;
            pl.player_num = p;
            GGPOPlayerHandle h;
            pl.type = p == 1 ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
            if (p == 2) {
                strcpy(pl.u.remote.ip_address, "127.0.0.1");
                pl.u.remote.port = (unsigned short)(port + 1);
            }
            ggpo_add_player(gS, &pl, &h);
        }
        for (int i = 0; i < 50; ++i) {
            ggpo_idle(gS, 1);
            Sleep(4);
        }
        ggpo_close_session(gS);
        gS = nullptr;
        printf("bindtest pass %d: session idled 200 ms and closed, no assertion\n", pass);
        fflush(stdout);
    }
    return 0;
}
#endif
}  // namespace

int main(int argc, char** argv) {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    timeBeginPeriod(1);
    ggpo_bind_loopback = 1;
    ggpo_after_send = CountSend;
#ifndef OLDLIB
    if (argc == 3 && !strcmp(argv[1], "bindtest")) return BindTest((unsigned short)atoi(argv[2]));
#endif
    if (argc < 5) {
        printf("usage: peer_test <localport> <remoteport> <player 1|2> <seconds> [bye=0|1] [timeout=ms] [delay=N] [norewind=0|1] "
               "[hashframes=N] [churn=0|1]\n");
        return 2;
    }
    const unsigned short localPort = (unsigned short)atoi(argv[1]);
    const int remotePort = atoi(argv[2]), me = atoi(argv[3]);
    const double seconds = atof(argv[4]);
    const int bye = Opt(argc, argv, "bye", 0), timeout = Opt(argc, argv, "timeout", 3000), delay = Opt(argc, argv, "delay", 0);
    const int hashFrames = Opt(argc, argv, "hashframes", 0), churn = Opt(argc, argv, "churn", 0);
    ggpo_no_disconnect_rewind = Opt(argc, argv, "norewind", 1);
#ifndef OLDLIB
    ggpo_bye_on_close = bye;
    const char* lib = "new";
#else
    const char* lib = "old";
#endif
    GGPOSessionCallbacks cb = Callbacks();
    char name[] = "MK9Rollback";
    if (ggpo_start_session(&gS, &cb, name, 2, sizeof(unsigned int), localPort) != GGPO_OK) {
        printf("start_session failed\n");
        return 1;
    }
#ifndef OLDLIB
    printf("T=%.1f player %d (%s library) on port %u, bye=%d, timeout=%d, delay=%d, ggpo_bind_failed=%d\n", NowMs(), me, lib, localPort, bye,
           timeout, delay, ggpo_bind_failed);
#else
    printf("T=%.1f player %d (%s library) on port %u, timeout=%d, delay=%d\n", NowMs(), me, lib, localPort, timeout, delay);
#endif
    ggpo_set_disconnect_timeout(gS, timeout);
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
            strcpy(pl.u.remote.ip_address, "127.0.0.1");
            pl.u.remote.port = (unsigned short)remotePort;
            ggpo_add_player(gS, &pl, &h);
        }
    }
    srand(12345 * me);
    unsigned int mask = 0;
    int hold = 0;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    double next = 0;
    int lastReport = 0, barrier = 0;
    for (;;) {
        QueryPerformanceCounter(&t);
        const double now = double(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now > seconds) break;
        ggpo_idle(gS, 1);
        if (now < next) {
            Sleep(1);
            continue;
        }
        next += 1.0 / 60.0;
        if (--hold <= 0) {  // any of the 32 bits, held 1-6 frames: every button index goes through the bit stream
            mask = ((unsigned)rand() << 17) ^ ((unsigned)rand() << 5) ^ (unsigned)rand();
            if (rand() % 4 == 0) mask = 0;
            hold = 1 + rand() % 6;
        }
        if (gRunning) {
            if (churn) mask = Churn(gFrame);  // the frame this input is added for (delay 0)
            const GGPOErrorCode rc = ggpo_add_local_input(gS, gLocal, &mask, sizeof mask);
            if (GGPO_SUCCEEDED(rc)) {
                unsigned int in[2];
                int disc = 0;
                if (GGPO_SUCCEEDED(ggpo_synchronize_input(gS, in, sizeof in, &disc))) {
                    Record(in);
                    ++gFrame;
                    ggpo_advance_frame(gS);
                }
            } else if (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD) {
                ++barrier;
            }
        }
        if ((int)now != lastReport) {
            lastReport = (int)now;
            GGPONetworkStats st{};
            for (int h = 1; h <= 2; ++h)
                if (h != gLocal && GGPO_SUCCEEDED(ggpo_get_network_stats(gS, h, &st)))
                    printf("t=%ds frame=%d ping=%dms behind local=%d remote=%d loads=%d barrier=%d kbps=%d pending=%d\n", lastReport, gFrame,
                           st.network.ping, st.timesync.local_frames_behind, st.timesync.remote_frames_behind, gLoads, barrier,
                           st.network.kbps_sent, st.network.send_queue_len);
            fflush(stdout);
        }
    }
    printf("T=%.1f closing (frame %d, running %d, disconnected %d)\n", NowMs(), gFrame, gRunning ? 1 : 0, gDisconnected ? 1 : 0);
    fflush(stdout);
    const int sendsBefore = gAfterSend;
    ggpo_close_session(gS);
    printf("T=%.1f closed: %d datagram(s) sent inside ggpo_close_session (ggpo_after_send calls)\n", NowMs(), gAfterSend - sendsBefore);
    int remoteNonZero = 0;
    const int other = me == 1 ? 1 : 0;
    for (int i = 0; i < gFrame && i < kRec; ++i) remoteNonZero += gRec[i][other] != 0;
    if (hashFrames > 0 && hashFrames <= gFrame) {
        unsigned int h = 2166136261u;
        for (int i = 0; i < hashFrames; ++i)
            for (int k = 0; k < 2; ++k)
                for (int b = 0; b < 4; ++b) h = (h ^ ((gRec[i][k] >> (8 * b)) & 0xFF)) * 16777619u;
        printf("inputs of frames 0-%d (both players): hash %08X; frame %d = %08X %08X\n", hashFrames - 1, h, hashFrames - 1,
               gRec[hashFrames - 1][0], gRec[hashFrames - 1][1]);
    }
#ifndef OLDLIB
    const int syncRtt = ggpo_sync_rtt_ms;
#else
    const int syncRtt = -1;
#endif
    printf("done: %d frames, %d loads, %d barrier waits, remote input non-zero in %d frames, disconnected=%d, sync_rtt=%dms\n", gFrame,
           gLoads, barrier, remoteNonZero, gDisconnected ? 1 : 0, syncRtt);
    return 0;
}
