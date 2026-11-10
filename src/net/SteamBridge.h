#pragma once
#include <cstdint>

// Milestone 15: GGPO over Steam P2P networking (NAT traversal / relay without port forwarding).
// GGPO keeps using UDP, but its "remote" is a local bridge socket on 127.0.0.1. The bridge forwards each
// datagram to the peer's SteamID with ISteamNetworking::SendP2PPacket on a private channel, and delivers
// packets read from that channel back to the local port. A second link carries the desync checksums.
// Uses the steam_api.dll the game already loaded (SteamNetworking005 / SteamUser016); nothing of the
// game's own online mode or matchmaking is touched.
namespace SteamBridge {
bool Available();       // steam_api.dll loaded and both interfaces resolved
uint64_t SelfId();      // this account's SteamID64 (0 if unavailable)
struct LinkSpec {
    unsigned short bridgePort;  // local bridge socket (127.0.0.1), the address GGPO / the checksum channel send to
    unsigned short localPort;   // where packets from the peer are delivered (GGPO's / the checksum socket's port)
    int channel;                // Steam P2P channel
    // Start drops what is pending on the channel (an earlier session's leftovers: the packets carry no session number).
    // true keeps it: for a link whose protocol rejects another session's packets itself (GGPO's magic numbers), so the
    // other game's first sync request, sent before this game started, is answered at once.
    bool keepPending = false;
};
// Reads and discards everything pending on each link's channel (unless keepPending), then opens the links.
bool Start(uint64_t peer, const LinkSpec* links, int count);
void Pump();  // forward pending packets both ways; call often (frame boundary, barrier waits)
// Only the local -> peer half (rc25): GGPO calls it after each datagram it sends (ggpo_after_send), so a send made inside
// a poll leaves at once instead of after the rest of the boundary. Never calls GGPO; a no-op when stopped.
void PumpOut();
void Stop();
// Online rooms (OnlineMode): the P2P session with the peer belongs to the game's own connection, so Stop must not close it.
void SetSharedSession(bool shared);
// Single datagrams on a private channel (the online-mode handshake), and the session state with a user.
bool SendRaw(uint64_t peer, const void* data, uint32_t size, int channel);
int ReadRaw(int channel, void* buf, uint32_t cap, uint64_t* from);  // bytes read, or -1 when nothing is pending
bool SessionActive(uint64_t peer);  // ISteamNetworking::GetP2PSessionState: a connection with that user is up
bool UsingRelay(uint64_t peer);     // the same call: that connection is up and goes through Steam's relay servers
// The same state as log text: active/connecting, Steam's session error (timeout, ...), relay, queued bytes/packets.
void DescribeSession(uint64_t peer, char* out, unsigned cap);
// This PC's default route (GetBestInterfaceEx towards a public IPv4 address: the routing table only, nothing is sent)
// leaves through a wireless adapter (GetAdaptersAddresses, IfType 802.11): 1 yes, 0 no, -1 unknown (any failure). A VPN
// or a virtual switch in front of the Wi-Fi adapter reads as 0. Slow (milliseconds, more on some systems): never on the
// game thread. Needs iphlpapi (-liphlpapi).
int DefaultRouteOnWifi();
}  // namespace SteamBridge
