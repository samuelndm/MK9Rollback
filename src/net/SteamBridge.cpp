#include "SteamBridge.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <cstdio>

#include "../debug/Logger.h"

namespace {
// ISteamNetworking (SteamNetworking005) vtable slots, thiscall; CSteamID is 8 bytes by value.
using SendP2PFn = bool(__thiscall*)(void* self, uint64_t remote, const void* data, uint32_t size, int sendType, int channel);
using AvailableFn = bool(__thiscall*)(void* self, uint32_t* size, int channel);
using ReadP2PFn = bool(__thiscall*)(void* self, void* dest, uint32_t cap, uint32_t* size, uint64_t* remote, int channel);
using CloseSessionFn = bool(__thiscall*)(void* self, uint64_t remote);
struct P2PSessionState {  // P2PSessionState_t
    uint8_t connectionActive, connecting, sessionError, usingRelay;
    int32_t bytesQueued, packetsQueued;
    uint32_t remoteIp;
    uint16_t remotePort;
};
using SessionStateFn = bool(__thiscall*)(void* self, uint64_t remote, P2PSessionState* out);  // slot 6
// ISteamUser (SteamUser016) slot 2: CSteamID GetSteamID() - class return through a hidden pointer.
using GetSteamIdFn = uint64_t*(__thiscall*)(void* self, uint64_t* ret);
constexpr int kSendUnreliableNoDelay = 1;

void* gNet = nullptr;
void* gUser = nullptr;
bool gResolved = false;
uint64_t gPeer = 0;
struct Link {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in local{};  // delivery address (127.0.0.1:localPort)
    int channel = 0;
};
Link gLinks[4];
int gLinkCount = 0;
unsigned gSent = 0, gReceived = 0, gForeign = 0;
bool gShared = false;

void* Vfn(void* obj, int slot) { return (*reinterpret_cast<void***>(obj))[slot]; }

bool Resolve() {
    if (gResolved) return gNet && gUser;
    gResolved = true;
    HMODULE api = GetModuleHandleA("steam_api.dll");
    if (!api) return false;
    using Accessor = void*(__cdecl*)();
    auto net = reinterpret_cast<Accessor>(GetProcAddress(api, "SteamNetworking"));
    auto user = reinterpret_cast<Accessor>(GetProcAddress(api, "SteamUser"));
    gNet = net ? net() : nullptr;
    gUser = user ? user() : nullptr;
    return gNet && gUser;
}

// Reads and drops what is pending on a channel (bounded: a peer that keeps sending must not hold the start).
unsigned Drain(int channel) {
    auto avail = reinterpret_cast<AvailableFn>(Vfn(gNet, 1));
    auto read = reinterpret_cast<ReadP2PFn>(Vfn(gNet, 2));
    char buf[2048];
    unsigned n = 0;
    uint32_t size = 0;
    while (n < 4096 && avail(gNet, &size, channel)) {
        uint64_t from = 0;
        uint32_t got = 0;
        if (!read(gNet, buf, sizeof buf, &got, &from, channel)) break;
        ++n;
    }
    return n;
}
}  // namespace

namespace SteamBridge {
bool Available() { return Resolve(); }

uint64_t SelfId() {
    if (!Resolve()) return 0;
    uint64_t id = 0;
    reinterpret_cast<GetSteamIdFn>(Vfn(gUser, 2))(gUser, &id);
    return id;
}

bool Start(uint64_t peer, const LinkSpec* links, int count) {
    Stop();
    if (!Resolve()) {
        Logger::Info("STEAM bridge: steam_api.dll interfaces unavailable");
        return false;
    }
    gPeer = peer;
    gSent = gReceived = gForeign = 0;
    // A session on the game's own Steam connection is not closed between fights (Stop), and the links' packets carry no
    // session number: what an earlier session left unread on a channel (a checksum of a session that stopped in its first
    // second, read as this session's frame) is dropped here, before the sockets exist. When the other game started its
    // session first, its first GGPO sync request goes with them: GGPO sends it again when this game's own request
    // arrives (the sync kick, tools\build_ggpo.ps1), about half a round trip later than it would have been answered.
    // LinkSpec::keepPending leaves a link's channel as it is.
    unsigned dropped = 0;
    for (int i = 0; i < count && i < 4; ++i)
        if (!links[i].keepPending) dropped += Drain(links[i].channel);
    if (dropped) Logger::Info("STEAM bridge: %u packet(s) an earlier session left on the links' channels dropped", dropped);
    for (int i = 0; i < count && i < 4; ++i) {
        Link& l = gLinks[i];
        l.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in b{};
        b.sin_family = AF_INET;
        b.sin_port = htons(links[i].bridgePort);
        b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        u_long nb = 1;
        if (l.s == INVALID_SOCKET || bind(l.s, reinterpret_cast<sockaddr*>(&b), sizeof b) || ioctlsocket(l.s, FIONBIO, &nb)) {
            Logger::Info("STEAM bridge: could not bind 127.0.0.1:%u", links[i].bridgePort);
            if (l.s != INVALID_SOCKET) closesocket(l.s);
            l.s = INVALID_SOCKET;
            Stop();
            return false;
        }
        // A datagram to a loopback port nobody has bound yet (the session's start) must not make the next recv fail
        // (SIO_UDP_CONNRESET: Windows' "port unreachable" report on UDP sockets). Failure is harmless (ForwardOut skips it).
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(l.s, 0x9800000C /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
        l.local = {};
        l.local.sin_family = AF_INET;
        l.local.sin_port = htons(links[i].localPort);
        l.local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        l.channel = links[i].channel;
        gLinkCount = i + 1;
    }
    Logger::Info("STEAM bridge: self %llu <-> peer %llu, %d links", (unsigned long long)SelfId(), (unsigned long long)peer, gLinkCount);
    return true;
}

// The datagrams waiting on one link's loopback socket go to the peer. A recv error other than "no more data" is skipped,
// not taken as the end: WSAECONNRESET reports an earlier datagram that found no listener (SIO_UDP_CONNRESET is off on
// these sockets, Start, but older systems may still report it) and the queued ones behind it are fine.
void ForwardOut(Link& l, char* buf, int cap) {
    auto send = reinterpret_cast<SendP2PFn>(Vfn(gNet, 0));
    for (int n = 0; n < 64; ++n) {  // local -> peer
        const int got = recv(l.s, buf, cap, 0);
        if (got <= 0) {
            if (got < 0 && WSAGetLastError() == WSAECONNRESET) continue;
            break;
        }
        if (send(gNet, gPeer, buf, (uint32_t)got, kSendUnreliableNoDelay, l.channel)) ++gSent;
    }
}

void Pump() {
    if (!gLinkCount || !gNet) return;
    auto avail = reinterpret_cast<AvailableFn>(Vfn(gNet, 1));
    auto read = reinterpret_cast<ReadP2PFn>(Vfn(gNet, 2));
    char buf[2048];
    for (int i = 0; i < gLinkCount; ++i) {
        Link& l = gLinks[i];
        if (l.s == INVALID_SOCKET) continue;
        ForwardOut(l, buf, sizeof buf);
        uint32_t size = 0;
        for (int n = 0; n < 64 && avail(gNet, &size, l.channel); ++n) {  // peer -> local
            uint64_t from = 0;
            uint32_t got = 0;
            if (!read(gNet, buf, sizeof buf, &got, &from, l.channel)) break;
            if (from != gPeer) {
                ++gForeign;
                continue;
            }
            if (got > sizeof buf) got = sizeof buf;  // the size Steam reports is the sender's: never past the buffer
            sendto(l.s, buf, (int)got, 0, reinterpret_cast<const sockaddr*>(&l.local), sizeof l.local);
            ++gReceived;
        }
    }
}

void PumpOut() {
    if (!gLinkCount || !gNet) return;
    char buf[2048];
    for (int i = 0; i < gLinkCount; ++i)
        if (gLinks[i].s != INVALID_SOCKET) ForwardOut(gLinks[i], buf, sizeof buf);
}

void Stop() {
    if (!gLinkCount) return;
    for (int i = 0; i < gLinkCount; ++i)
        if (gLinks[i].s != INVALID_SOCKET) {
            closesocket(gLinks[i].s);
            gLinks[i].s = INVALID_SOCKET;
        }
    if (gNet && gPeer && !gShared) reinterpret_cast<CloseSessionFn>(Vfn(gNet, 4))(gNet, gPeer);
    Logger::Info("STEAM bridge stopped: %u packets sent, %u received, %u from other users ignored", gSent, gReceived, gForeign);
    gLinkCount = 0;
    gPeer = 0;
}

void SetSharedSession(bool shared) { gShared = shared; }

bool SendRaw(uint64_t peer, const void* data, uint32_t size, int channel) {
    if (!Resolve() || !peer) return false;
    return reinterpret_cast<SendP2PFn>(Vfn(gNet, 0))(gNet, peer, data, size, kSendUnreliableNoDelay, channel);
}

int ReadRaw(int channel, void* buf, uint32_t cap, uint64_t* from) {
    if (!Resolve()) return -1;
    uint32_t size = 0;
    if (!reinterpret_cast<AvailableFn>(Vfn(gNet, 1))(gNet, &size, channel)) return -1;
    uint32_t got = 0;
    if (!reinterpret_cast<ReadP2PFn>(Vfn(gNet, 2))(gNet, buf, cap, &got, from, channel)) return -1;
    return (int)(got < cap ? got : cap);  // never more than was written to buf, whatever size the packet claimed
}

bool SessionActive(uint64_t peer) {
    if (!Resolve() || !peer) return false;
    P2PSessionState s{};
    return reinterpret_cast<SessionStateFn>(Vfn(gNet, 6))(gNet, peer, &s) && s.connectionActive;
}

bool UsingRelay(uint64_t peer) {
    if (!Resolve() || !peer) return false;
    P2PSessionState s{};
    return reinterpret_cast<SessionStateFn>(Vfn(gNet, 6))(gNet, peer, &s) && s.connectionActive && s.usingRelay;
}

int DefaultRouteOnWifi() {
    // The interface of the route to a public IPv4 address: read from the routing table, nothing is sent.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(0x08080808u);
    DWORD index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&to), &index) != NO_ERROR) return -1;
    ULONG size = 16 * 1024;
    for (int attempt = 0; attempt < 3; ++attempt) {  // the list can grow between the size and the data
        IP_ADAPTER_ADDRESSES* list = static_cast<IP_ADAPTER_ADDRESSES*>(HeapAlloc(GetProcessHeap(), 0, size));
        if (!list) return -1;
        const ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                             list, &size);
        int wifi = -1;
        if (r == NO_ERROR)
            for (const IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next)
                if (a->IfIndex == index) {
                    wifi = a->IfType == IF_TYPE_IEEE80211 ? 1 : 0;
                    break;
                }
        HeapFree(GetProcessHeap(), 0, list);
        if (r != ERROR_BUFFER_OVERFLOW) return wifi;
    }
    return -1;
}

void DescribeSession(uint64_t peer, char* out, unsigned cap) {
    P2PSessionState s{};
    if (!Resolve() || !peer || !reinterpret_cast<SessionStateFn>(Vfn(gNet, 6))(gNet, peer, &s)) {
        snprintf(out, cap, "no session with %llu", (unsigned long long)peer);
        return;
    }
    static const char* const kError[] = {"none", "their game is not running", "no rights to the app", "not logged in", "timeout"};
    snprintf(out, cap, "active %u, connecting %u, error %u (%s), relay %u, queued %d bytes / %d packets", s.connectionActive,
             s.connecting, s.sessionError, s.sessionError < 5 ? kError[s.sessionError] : "?", s.usingRelay, s.bytesQueued,
             s.packetsQueued);
}
}  // namespace SteamBridge
