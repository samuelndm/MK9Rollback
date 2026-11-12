// Loopback relay between two ggpo_peer instances, for the handshake's round trip (build_ggpo.ps1, rc30, 7): a transport
// whose far end starts reading late. Peer A plays against <relay port A>, peer B against <relay port B> (both 127.0.0.1):
//   ggpo_relay <port A> <relay port A> <port B> <relay port B> <one-way delay ms> <seconds>
// Every datagram is forwarded <delay> ms after it arrived, in order. A's datagrams are also held until B's first datagram
// has arrived (B's game has started: the queue is read now), so A's first sync requests are answered late: their replies
// measure the wait, and only the reply to A's newest request (sent when B's own request arrives) measures 2 x delay.
// Run: the relay, peer A, <wait>, peer B; ggpo_peer's last line ends with sync_rtt (the library's ggpo_sync_rtt_ms).
#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {
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
SOCKET Open(int port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a)) {
        printf("relay: bind %d failed (%d)\n", port, WSAGetLastError());
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL off = FALSE;  // no WSAECONNRESET when a peer's port is not open (yet, or any more)
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    return s;
}
struct Held {
    double due;
    std::vector<char> data;
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        printf("usage: ggpo_relay <port A> <relay port A> <port B> <relay port B> <one-way delay ms> <seconds>\n");
        return 2;
    }
    const int port[2] = {atoi(argv[1]), atoi(argv[3])}, relayPort[2] = {atoi(argv[2]), atoi(argv[4])};
    const double delay = atof(argv[5]), seconds = atof(argv[6]);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    timeBeginPeriod(1);
    SOCKET sock[2] = {Open(relayPort[0]), Open(relayPort[1])};  // [0]: A's remote, [1]: B's remote
    if (sock[0] == INVALID_SOCKET || sock[1] == INVALID_SOCKET) return 1;
    sockaddr_in to[2]{};
    for (int i = 0; i < 2; ++i) {
        to[i].sin_family = AF_INET;
        to[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to[i].sin_port = htons((unsigned short)port[i]);
    }
    std::deque<Held> queue[2];  // [0]: from A, for B; [1]: from B, for A
    double firstSeen[2] = {-1, -1}, longestHold = 0;
    int forwarded[2] = {0, 0}, heldForB = 0;
    const double t0 = NowMs();
    while (NowMs() - t0 < seconds * 1000.0) {
        for (int i = 0; i < 2; ++i) {
            for (;;) {
                char buf[8192];
                sockaddr_in from{};
                int fl = sizeof from;
                const int len = recvfrom(sock[i], buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                if (len < 0) break;
                if (from.sin_port != to[i].sin_port) continue;  // not the peer this socket stands in for
                const double now = NowMs();
                if (firstSeen[i] < 0) firstSeen[i] = now;
                queue[i].push_back(Held{now + delay, std::vector<char>(buf, buf + len)});
            }
        }
        const double now = NowMs();
        for (int i = 0; i < 2; ++i) {
            if (i == 0 && firstSeen[1] < 0) continue;  // B has not started: A's datagrams wait
            while (!queue[i].empty() && queue[i].front().due <= now) {
                const Held& h = queue[i].front();
                if (i == 0 && now - h.due > 1.5) {
                    ++heldForB;
                    if (now - h.due > longestHold) longestHold = now - h.due;
                }
                sendto(sock[1 - i], h.data.data(), (int)h.data.size(), 0, reinterpret_cast<sockaddr*>(&to[1 - i]), sizeof to[1 - i]);
                ++forwarded[i];
                queue[i].pop_front();
            }
        }
        Sleep(1);
    }
    printf("relay done: delay %.0f ms each way; B's first datagram %.0f ms after A's; %d of A's datagrams waited for B (longest %.0f ms past "
           "their delay); forwarded %d from A, %d from B\n",
           delay, firstSeen[0] >= 0 && firstSeen[1] >= 0 ? firstSeen[1] - firstSeen[0] : -1.0, heldForB, longestHold, forwarded[0],
           forwarded[1]);
    return 0;
}
