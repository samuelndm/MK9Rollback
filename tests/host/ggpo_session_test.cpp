// In-process checks of the rc30 GGPO patches (tools\build_ggpo.ps1), no game involved. The library under test runs its
// sessions on 127.0.0.1:<port>; the remote is a socket of this process on <port + 1> that speaks just enough of the wire
// protocol by hand (sync replies, keep-alives, quality replies), so every timing below is decided here.
//   ggpo_session_test [port]         all checks (default port 7411); exit code: the number of failed checks
//   ggpo_session_test first <port>   prints the magic number and the first sync random of this process's first session
// Checks (the numbers are the rc30 review's items):
//   6  two fresh processes do not start with the same magic number and sync random (stock: rand() without a seed);
//   4  network statistics run from the start of a process's first session (stock: the union's handshake random was read
//      as the time of the last statistics update);
//   1  a quality reply whose round trip would be negative or above 5000 ms changes neither the ping nor ggpo_rtt_min_ms;
//   7  a sync reply that arrives once the session runs still lowers ggpo_sync_rtt_ms when it answers a fresher request
//      (and raises no event);
//   3  closing a session returns its handles (stock: Poll's event handle stayed open);
//   5  the bytes no field owns leave as zero: a sync request's remote_magic / remote_endpoint, the bits after num_bits
//      in an input packet's last byte (stock: heap bytes of earlier packets).
#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ggponet.h"

extern "C" int ggpo_bind_loopback;
extern "C" int ggpo_no_disconnect_rewind;
extern "C" int ggpo_rtt_min_ms;
extern "C" int ggpo_sync_rtt_ms;

extern "C" void MK9R_GgpoSyncError(const char* msg) { printf("sync error: %s\n", msg); }
extern "C" void MK9R_GgpoAssert(const char* msg) {  // the library would call exit(0) right after
    printf("FAIL: GGPO ASSERT: %s\n", msg);
    fflush(stdout);
    ExitProcess(90);
}

namespace {
enum { T_SYNCREQ = 1, T_SYNCREP = 2, T_INPUT = 3, T_QREPORT = 4, T_QREPLY = 5, T_KEEPALIVE = 6 };
const int kInputFixed = 32;  // udp_msg.h, pack 1: hdr 5, connect_status[4] 16, start_frame 4, ack 4, num_bits 2, input_size 1

double NowMs() {
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}
uint16_t R16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint32_t gRng = 0x2545F491;
uint32_t Rnd() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 17;
    gRng ^= gRng << 5;
    return gRng;
}

// ---- the library's side --------------------------------------------------------------------------------------------
GGPOSession* gS = nullptr;
GGPOPlayerHandle gLocal = GGPO_INVALID_HANDLE, gRemotePlayer = GGPO_INVALID_HANDLE;
int gFrame = 0, gEvents = 0;
bool gRunning = false, gDisconnected = false;

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
    ++gEvents;
    if (e->code == GGPO_EVENTCODE_RUNNING) gRunning = true;
    if (e->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) gDisconnected = true;
    return true;
}

// ---- the hand-made remote ------------------------------------------------------------------------------------------
SOCKET gSock = INVALID_SOCKET;
sockaddr_in gLib;
struct Remote {
    uint16_t magic = 0, seq = 0;
    bool replySync = true;  // answer each sync request at once
    int quality = 0;        // quality reports: 0 unanswered, 1 echoed, 2 pong far in the past / in the future (alternating)
    std::vector<uint32_t> requests;  // the library's sync randoms, as they arrived
    double firstRequestMs = -1;
    uint16_t libMagic = 0;
    int reports = 0, badReplies = 0;
    bool synced = false;
} R;
int gSyncRequests = 0, gDirtySync = 0, gInputPackets = 0, gInputWithBits = 0, gDirtyInput = 0, gMaxBits = 0;

void RemoteSend(int type, const void* payload, int n) {
    uint8_t p[64] = {};
    memcpy(p, &R.magic, 2);
    memcpy(p + 2, &R.seq, 2);
    ++R.seq;
    p[4] = (uint8_t)type;
    if (n) memcpy(p + 5, payload, n);
    sendto(gSock, reinterpret_cast<const char*>(p), 5 + n, 0, reinterpret_cast<sockaddr*>(&gLib), sizeof gLib);
}
void RemoteSyncReply(uint32_t random) {
    RemoteSend(T_SYNCREP, &random, 4);
    R.synced = true;
}
void RemotePump() {
    for (int resets = 0;;) {
        uint8_t b[8192];
        sockaddr_in from{};
        int fl = sizeof from;
        const int len = recvfrom(gSock, reinterpret_cast<char*>(b), sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (len < 0) {
            if (WSAGetLastError() == WSAECONNRESET && ++resets < 64) continue;
            return;
        }
        if (len < 5 || from.sin_port != gLib.sin_port) continue;
        switch (b[4]) {
            case T_SYNCREQ:
                if (len < 12) break;
                ++gSyncRequests;
                if (b[9] | b[10] | b[11]) ++gDirtySync;  // remote_magic, remote_endpoint: no code sets them
                if (R.firstRequestMs < 0) R.firstRequestMs = NowMs();
                R.libMagic = R16(b);
                R.requests.push_back(R32(b + 5));
                if (R.replySync) RemoteSyncReply(R32(b + 5));
                break;
            case T_QREPORT: {
                if (len < 10) break;
                const uint32_t ping = R32(b + 6);
                ++R.reports;
                uint32_t pong = ping;
                if (R.quality == 2) pong = (++R.badReplies & 1) ? ping - 100000000u : ping + 60000u;
                if (R.quality) RemoteSend(T_QREPLY, &pong, 4);
                break;
            }
            case T_INPUT: {
                if (len < kInputFixed) break;
                const int nb = R16(b + 29);
                ++gInputPackets;
                if (nb > gMaxBits) gMaxBits = nb;
                if (nb && len >= kInputFixed + (nb + 7) / 8) {
                    ++gInputWithBits;
                    if (nb % 8 && (b[kInputFixed + nb / 8] >> (nb % 8))) ++gDirtyInput;  // the bits after the stream's end
                }
                break;
            }
            default: break;
        }
    }
}

// ---- sessions ------------------------------------------------------------------------------------------------------
int gPort = 7411, gSessions = 0, gFailed = 0, gChecks = 0;

void Check(bool ok, const char* what) {
    ++gChecks;
    if (!ok) ++gFailed;
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}
void StartSession() {
    GGPOSessionCallbacks cb{};
    cb.begin_game = BeginGame;
    cb.save_game_state = SaveState;
    cb.load_game_state = LoadState;
    cb.log_game_state = LogState;
    cb.free_buffer = FreeBuffer;
    cb.advance_frame = AdvanceFrame;
    cb.on_event = OnEvent;
    char name[] = "MK9Rollback";
    R = Remote();
    R.magic = (uint16_t)(0x4000 + gSessions++);
    gFrame = gEvents = 0;
    gRunning = gDisconnected = false;
    ggpo_start_session(&gS, &cb, name, 2, sizeof(unsigned int), (unsigned short)gPort);
    for (int p = 1; p <= 2; ++p) {
        GGPOPlayer pl{};
        pl.size = sizeof pl;
        pl.player_num = p;
        GGPOPlayerHandle h;
        if (p == 1) {
            pl.type = GGPO_PLAYERTYPE_LOCAL;
            ggpo_add_player(gS, &pl, &h);
            gLocal = h;
        } else {
            pl.type = GGPO_PLAYERTYPE_REMOTE;
            strcpy(pl.u.remote.ip_address, "127.0.0.1");
            pl.u.remote.port = (unsigned short)(gPort + 1);
            ggpo_add_player(gS, &pl, &h);
            gRemotePlayer = h;
        }
    }
}
void Step() {
    ggpo_idle(gS, 0);
    RemotePump();
}
void CloseSession() {
    ggpo_close_session(gS);
    gS = nullptr;
    RemotePump();
}
// One frame with this local input, if the library takes it (it stops 8 frames past the remote's, which sends none).
void TryFrame(unsigned int mask) {
    if (!gRunning || !GGPO_SUCCEEDED(ggpo_add_local_input(gS, gLocal, &mask, sizeof mask))) return;
    unsigned int in[2];
    int disc = 0;
    if (GGPO_SUCCEEDED(ggpo_synchronize_input(gS, in, sizeof in, &disc))) {
        ++gFrame;
        ggpo_advance_frame(gS);
    }
}
bool OpenRemote() {
    gSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)(gPort + 1));
    if (bind(gSock, reinterpret_cast<sockaddr*>(&a), sizeof a)) {
        printf("FAIL: the remote could not bind %d (%d)\n", gPort + 1, WSAGetLastError());
        return false;
    }
    u_long nb = 1;
    ioctlsocket(gSock, FIONBIO, &nb);
    gLib = a;
    gLib.sin_port = htons((unsigned short)gPort);
    return true;
}

// 'first <port>': the magic number and the first sync random of this process's first session.
int First() {
    if (!OpenRemote()) return 1;
    StartSession();
    R.replySync = false;
    for (int i = 0; i < 500 && R.requests.empty(); ++i) {
        Step();
        Sleep(1);
    }
    if (R.requests.empty()) {
        printf("first: no sync request\n");
        return 1;
    }
    printf("first: magic=%u random=%u\n", R.libMagic, R.requests[0]);
    CloseSession();
    return 0;
}
// Runs this program again with 'first' and returns its "magic=... random=..." text ("" on failure).
void RunFirst(char* out, int cap) {
    out[0] = 0;
    char exe[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" first %d", exe, gPort);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (started) {
        char buf[512];
        int n = 0;
        DWORD got = 0;
        while (n < (int)sizeof buf - 1 && ReadFile(rd, buf + n, sizeof buf - 1 - n, &got, nullptr) && got) n += (int)got;
        buf[n] = 0;
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        const char* m = strstr(buf, "magic=");
        if (m) {
            snprintf(out, cap, "%s", m);
            for (char* c = out; *c; ++c)
                if (*c == '\r' || *c == '\n') *c = 0;
        }
    }
    CloseHandle(rd);
}
}  // namespace

int main(int argc, char** argv) {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    timeBeginPeriod(1);
    ggpo_bind_loopback = 1;
    ggpo_no_disconnect_rewind = 1;
    if (argc == 3 && !strcmp(argv[1], "first")) {
        gPort = atoi(argv[2]);
        return First();
    }
    if (argc > 1) gPort = atoi(argv[1]);
    char line[600];

    // 6: before this process opens its own sockets.
    char a[200], b[200];
    RunFirst(a, sizeof a);
    RunFirst(b, sizeof b);
    snprintf(line, sizeof line, "item 6: two fresh processes start with different numbers (%s | %s)", a[0] ? a : "no output", b[0] ? b : "no output");
    Check(a[0] && b[0] && strcmp(a, b) != 0, line);

    if (!OpenRemote()) return 1;

    // 4 and 1: this process's first session. The remote answers the handshake at once and sends a keep-alive every
    // 100 ms; the library adds inputs (8 frames are taken, then resent every few ms: enough bytes for the statistics).
    {
        StartSession();
        double runningAt = -1, kbpsAt = -1, nextTick = NowMs(), nextAlive = 0;
        int minA = 0, pingA = 0, minB = 0, pingB = 0, minC = 0, pingC = 0, phase = 0, kbps = 0;
        for (const double t0 = NowMs(); NowMs() - t0 < 6000 && phase < 4;) {
            Step();
            const double now = NowMs();
            if (gRunning && runningAt < 0) runningAt = now;
            if (R.synced && now >= nextAlive) {
                RemoteSend(T_KEEPALIVE, nullptr, 0);
                nextAlive = now + 100;
            }
            if (now >= nextTick) {
                nextTick += 1000.0 / 60.0;
                TryFrame(Rnd());
            }
            if (runningAt >= 0) {
                GGPONetworkStats st{};
                ggpo_get_network_stats(gS, gRemotePlayer, &st);
                if (kbpsAt < 0 && st.network.kbps_sent > 0) {
                    kbpsAt = now - runningAt;
                    kbps = st.network.kbps_sent;
                }
                const double t = now - runningAt;
                if (phase == 0) {  // 0-1000 ms: every quality reply is out of range
                    R.quality = 2;
                    if (t >= 1000) { minA = ggpo_rtt_min_ms; pingA = st.network.ping; phase = 1; R.quality = 1; }
                } else if (phase == 1) {  // 1000-1600 ms: echoed as a GGPO peer does
                    if (t >= 1600) { minB = ggpo_rtt_min_ms; pingB = st.network.ping; phase = 2; R.quality = 2; }
                } else if (phase == 2) {  // 1600-2200 ms: out of range again
                    if (t >= 2200) { minC = ggpo_rtt_min_ms; pingC = st.network.ping; phase = 3; R.quality = 1; }
                } else if (t >= 3000) {
                    phase = 4;
                }
            }
            Sleep(1);
        }
        snprintf(line, sizeof line, "item 4: the first session's statistics run from its start (kbps_sent %d after %.0f ms of running; -1: never in 3 s)",
                 kbps, kbpsAt);
        Check(phase == 4 && kbpsAt >= 0 && kbpsAt <= 2500, line);
        snprintf(line, sizeof line, "item 1: %d quality replies out of range (round trip 100000000 ms / -60000 ms) leave ping %d, ggpo_rtt_min_ms %d (wanted 0, -1)",
                 R.badReplies, pingA, minA);
        Check(phase == 4 && R.badReplies >= 4 && pingA == 0 && minA == -1, line);
        snprintf(line, sizeof line, "item 1: echoed quality replies are taken (ping %d, ggpo_rtt_min_ms %d; wanted 0-200)", pingB, minB);
        Check(phase == 4 && pingB >= 0 && pingB <= 200 && minB >= 0 && minB <= 200, line);
        snprintf(line, sizeof line, "item 1: replies out of range after good ones change nothing (ping %d, ggpo_rtt_min_ms %d; wanted 0-200)", pingC, minC);
        Check(phase == 4 && pingC >= 0 && pingC <= 200 && minC >= 0 && minC <= 200, line);
        Check(!gDisconnected, "the session with the hand-made remote was not disconnected");
        CloseSession();
    }

    // 7: the remote stays silent for 400 ms (the library asks again every 150 ms), then sends its own sync request (the
    // library answers and asks once more at once: the sync kick), answers the library's OLDEST request (the handshake is
    // done, the round trip reads the 400 ms of waiting) and then the newest one.
    {
        StartSession();
        R.replySync = false;
        int state = 0, first = -2, second = -2, eventsBefore = 0, eventsAfter = 0;
        size_t before = 0;
        bool runningFirst = false;
        double mark = 0;
        for (const double t0 = NowMs(); NowMs() - t0 < 3000 && state < 4;) {
            Step();
            const double now = NowMs();
            if (state == 0 && R.firstRequestMs >= 0 && now - R.firstRequestMs >= 400 && R.requests.size() >= 3) {
                before = R.requests.size();
                const uint8_t mine[7] = {0x34, 0x12, 0, 0, 0, 0, 0};  // random_request, remote_magic, remote_endpoint
                RemoteSend(T_SYNCREQ, mine, sizeof mine);
                state = 1;
            } else if (state == 1 && R.requests.size() > before) {
                RemoteSyncReply(R.requests[0]);
                mark = now;
                state = 2;
            } else if (state == 2 && now - mark >= 5) {
                first = ggpo_sync_rtt_ms;
                runningFirst = gRunning;
                eventsBefore = gEvents;
                RemoteSyncReply(R.requests.back());
                mark = now;
                state = 3;
            } else if (state == 3 && now - mark >= 20) {
                second = ggpo_sync_rtt_ms;
                eventsAfter = gEvents;
                state = 4;
            }
            Sleep(1);
        }
        snprintf(line, sizeof line, "item 7: %d sync requests seen; reply to the oldest: running %d, ggpo_sync_rtt_ms %d (wanted 350-700)",
                 (int)R.requests.size(), runningFirst ? 1 : 0, first);
        Check(state == 4 && runningFirst && first >= 350 && first <= 700, line);
        snprintf(line, sizeof line, "item 7: the late reply to the newest request lowers it (ggpo_sync_rtt_ms %d, wanted 0-60)", second);
        Check(state == 4 && second >= 0 && second <= 60, line);
        snprintf(line, sizeof line, "item 7: the late reply raises no event and the session keeps running (events %d -> %d, running %d)", eventsBefore,
                 eventsAfter, gRunning ? 1 : 0);
        Check(state == 4 && eventsBefore == eventsAfter && gRunning && !gDisconnected, line);
        CloseSession();
    }

    // 3 and 5: 40 short sessions (handshake, 8 frames of random input, close) after one more to settle the counts.
    {
        DWORD h0 = 0, h1 = 0;
        const int kSessions = 40;
        for (int s = -1; s < kSessions; ++s) {
            if (s == 0) GetProcessHandleCount(GetCurrentProcess(), &h0);
            StartSession();
            for (int k = 0; k < 30; ++k) {
                Step();
                TryFrame(Rnd());
                Sleep(1);
            }
            CloseSession();
        }
        GetProcessHandleCount(GetCurrentProcess(), &h1);
        snprintf(line, sizeof line, "item 3: %d sessions started and closed: process handles %lu -> %lu (wanted: no more than 2 apart)", kSessions,
                 (unsigned long)h0, (unsigned long)h1);
        Check((long)h1 - (long)h0 <= 2 && (long)h0 - (long)h1 <= 2, line);
        snprintf(line, sizeof line, "item 5: %d sync requests, %d with a non-zero byte in remote_magic / remote_endpoint (wanted 0)", gSyncRequests,
                 gDirtySync);
        Check(gSyncRequests >= kSessions && gDirtySync == 0, line);
        snprintf(line, sizeof line, "item 5: %d input packets with bits (%d in all, longest %d bits), %d with a bit set after the stream's end (wanted 0)",
                 gInputWithBits, gInputPackets, gMaxBits, gDirtyInput);
        Check(gInputWithBits >= kSessions && gDirtyInput == 0, line);
    }

    printf("ggpo_session_test done: %d checks, failed %d\n", gChecks, gFailed);
    return gFailed;
}
