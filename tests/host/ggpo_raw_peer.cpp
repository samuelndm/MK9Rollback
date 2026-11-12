// Raw-socket GGPO remote for the round 3 validation test: speaks the wire protocol by hand (handshake, a valid 60 Hz
// input stream, acks, quality replies) from the address the peer under test expects its remote at, and injects malformed
// datagrams between the valid ones. After each group it checks that the peer still takes the valid stream (its ack of our
// frames moves on) and still sends its own frames.
//   raw_peer <myport> <peerport> <my player 1|2> <seconds> <case[,case...]|all|none> [seed]
// Cases: size numbits button unterminated halfrecord gap farstart status trunc type oversize random bitfuzz validfuzz flood
//        seqjump (a sync request with a foreign magic and a far sequence number: the real stream must still be taken)
//        slowack (45 frames without acknowledging anything new, then on: the peer's stream must go on without a gap)
//        presync (magic 0 input before the handshake) noack (never acknowledge: expects the peer to disconnect us)
//        churncheck (the peer runs with churn=1, delay 0: every decoded frame must be Churn(frame), see ggpo_peer.cpp)
// 'all' is every case of the first line and seqjump except validfuzz.
#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
enum { T_INVALID = 0, T_SYNCREQ = 1, T_SYNCREP = 2, T_INPUT = 3, T_QREPORT = 4, T_QREPLY = 5, T_KEEPALIVE = 6, T_INPUTACK = 7 };
// Wire layout (udp_msg.h, pack 1): hdr { u16 magic; u16 seq; u8 type } = 5 bytes, then for an input:
//   connect_status[4] { disconnected:1, last_frame:31 } @5, u32 start_frame @21, { disconnect_requested:1, ack_frame:31 } @25,
//   u16 num_bits @29, u8 input_size @31, bits @32.
const int kHdr = 5, kInputFixed = 32;

SOCKET gSock;
sockaddr_in gPeer;
uint16_t gMagic = 0x5A17, gSeq = 0;
int gMe = 1, gOther = 0;         // queue indexes
std::vector<uint32_t> gIn;       // my inputs by frame
int gMyFrame = 0;                // next frame to produce
int gPeerAck = -1;               // last frame of mine the peer acknowledged
int gPeerLast = -1;              // last frame of the peer I decoded
uint32_t gPeerBits = 0;
bool gSynced = false, gPeerBye = false, gNoAck = false;
uint32_t gMyRandom = 0;
int gPeerPackets = 0, gPeerInputPackets = 0, gGaps = 0, gSent = 0;
uint32_t gRng = 0x1234567;
int gAckFreeze = 0, gAckFrozen = -1;  // slowack: ticks left without a new acknowledgement, and the frame acknowledged meanwhile
bool gChurn = false;                  // churncheck
int gMismatch = 0, gFirstMismatch = -1;

// The input ggpo_peer adds for a frame with churn=1 (the same function there): about 16 of 32 bits change every frame.
uint32_t Churn(int frame) {
    uint32_t x = (uint32_t)frame * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    return x;
}
// The peer frame this side acknowledges now.
int AckFrame() { return gNoAck ? -1 : gAckFreeze > 0 ? gAckFrozen : gPeerLast; }

uint32_t Rnd() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 17;
    gRng ^= gRng << 5;
    return gRng;
}
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
void W16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
void W32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
uint16_t R16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

struct BitW {
    uint8_t* b;
    int off = 0;
    void Bit(int v) {
        if (v) b[off / 8] |= (uint8_t)(1 << (off % 8));
        else b[off / 8] &= (uint8_t)~(1 << (off % 8));
        ++off;
    }
    void Nib(int v) { for (int i = 0; i < 8; ++i) Bit((v >> i) & 1); }
    void Record(int on, int button) { Bit(1); Bit(on); Nib(button); }
};

void Send(const uint8_t* p, int len) {
    sendto(gSock, reinterpret_cast<const char*>(p), len, 0, reinterpret_cast<sockaddr*>(&gPeer), sizeof gPeer);
    ++gSent;
}
void Header(uint8_t* p, int type, uint16_t magic) {
    W16(p, magic);
    W16(p + 2, gSeq++);
    p[4] = (uint8_t)type;
}
// The fixed part of an input packet as a GGPO peer would send it now.
void InputHead(uint8_t* p, int start, int numBits, int size) {
    Header(p, T_INPUT, gMagic);
    for (int i = 0; i < 4; ++i) W32(p + 5 + 4 * i, 0xFFFFFFFEu);  // last_frame -1, connected
    W32(p + 5 + 4 * gMe, (uint32_t)(gMyFrame - 1) << 1);
    W32(p + 5 + 4 * gOther, (uint32_t)AckFrame() << 1);
    W32(p + 21, (uint32_t)start);
    W32(p + 25, (uint32_t)AckFrame() << 1);
    W16(p + 29, (uint16_t)numBits);
    p[31] = (uint8_t)size;
}
// Frames [from, to) of my inputs, compressed as SendPendingOutput does (relative to the frame before 'from').
int EncodeFrames(BitW& w, int from, int to) {
    uint32_t last = from > 0 ? gIn[from - 1] : 0;
    for (int f = from; f < to; ++f) {
        for (int i = 0; i < 32; ++i)
            if (((gIn[f] ^ last) >> i) & 1) w.Record((gIn[f] >> i) & 1, i);
        w.Bit(0);
        last = gIn[f];
    }
    return w.off;
}
// A valid input packet with every frame the peer has not acknowledged (at most 40).
int ValidInput(uint8_t* p) {
    memset(p, 0, 2048);
    int from = gPeerAck + 1, to = gMyFrame;
    if (to - from > 40) to = from + 40;
    BitW w{p + kInputFixed};
    if (from < to) {
        EncodeFrames(w, from, to);
        InputHead(p, from, w.off, 4);
    } else {
        InputHead(p, 0, 0, 0);
    }
    return kInputFixed + (w.off + 7) / 8;
}

void OnPeerInput(const uint8_t* p, int len) {
    if (len < kInputFixed) return;
    const uint32_t start = R32(p + 21), ackw = R32(p + 25);
    const int nb = R16(p + 29), ack = (int32_t)ackw >> 1;
    if (ackw & 1) gPeerBye = true;
    if (ack > gPeerAck) gPeerAck = ack;
    ++gPeerInputPackets;
    if (!nb || len < kInputFixed + (nb + 7) / 8) return;
    int cur = (int)start, off = 0;
    const uint8_t* b = p + kInputFixed;
    auto rb = [&]() { const int v = (b[off / 8] >> (off % 8)) & 1; ++off; return v; };
    if (cur > gPeerLast + 1) {  // the peer dropped frames we never saw (we acknowledged garbage): go on from here
        if (gPeerLast >= 0) ++gGaps;
        gPeerLast = cur - 1;
    }
    while (off < nb) {
        const bool use = cur == gPeerLast + 1;
        while (rb()) {
            const int on = rb();
            int button = 0;
            for (int i = 0; i < 8; ++i) button |= rb() << i;
            if (use && button < 32) gPeerBits = on ? (gPeerBits | (1u << button)) : (gPeerBits & ~(1u << button));
        }
        if (use) {
            gPeerLast = cur;
            if (gChurn && gPeerBits != Churn(cur)) {
                if (!gMismatch) gFirstMismatch = cur;
                ++gMismatch;
            }
        }
        ++cur;
    }
}
void Pump() {
    for (;;) {
        uint8_t buf[4096];
        sockaddr_in from{};
        int fl = sizeof from;
        const int len = recvfrom(gSock, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (len < kHdr) {
            if (len < 0 && WSAGetLastError() == WSAECONNRESET) continue;
            if (len < 0) return;
            continue;
        }
        if (from.sin_port != gPeer.sin_port) continue;
        ++gPeerPackets;
        uint8_t out[64] = {};
        switch (buf[4]) {
            case T_SYNCREQ:
                Header(out, T_SYNCREP, gMagic);
                memcpy(out + 5, buf + 5, 4);
                Send(out, 9);
                break;
            case T_SYNCREP:
                if (R32(buf + 5) == gMyRandom && !gSynced) {
                    gSynced = true;
                    printf("[%7.0f] handshake done (peer magic %u)\n", NowMs(), R16(buf));
                }
                break;
            case T_QREPORT:
                Header(out, T_QREPLY, gMagic);
                memcpy(out + 5, buf + 6, 4);
                Send(out, 9);
                break;
            case T_INPUT: OnPeerInput(buf, len); break;
            default: break;
        }
    }
}

// ---- malformed datagrams -------------------------------------------------------------------------------------------
typedef std::vector<std::vector<uint8_t>> Pkts;
// Every unacknowledged frame, then one new frame (number gMyFrame, not produced yet) made by 'extra'; the header is
// written by InputHead and may be altered by the caller.
std::vector<uint8_t> WithNewFrame(void (*extra)(BitW&), int size = 4, int cutBits = 0) {
    std::vector<uint8_t> v(2048, 0);
    BitW w{v.data() + kInputFixed};
    int from = gPeerAck + 1, to = gMyFrame;
    if (to - from > 40) from = to - 40;
    if (from < 0) from = 0;
    EncodeFrames(w, from, to);
    extra(w);
    const int bits = w.off - cutBits;
    InputHead(v.data(), from, bits, size);
    v.resize(kInputFixed + (bits + 7) / 8);
    return v;
}
void NewFrameOk(BitW& w) { w.Record(1, 3); w.Bit(0); }
void NewFrameButton200(BitW& w) { w.Record(1, 200); w.Bit(0); }
void NewFrameButton255(BitW& w) { w.Record(1, 255); w.Bit(0); }
void NewFrameButton32(BitW& w) { w.Record(1, 32); w.Bit(0); }
void NewFrameRecordOnly(BitW& w) { w.Record(1, 3); }

Pkts Case(const std::string& name) {
    Pkts out;
    if (name == "size") {  // a new frame with a size that is not the session's (stock: input_queue.cpp:221)
        for (int size : {5, 0, 255, 18, 1}) out.push_back(WithNewFrame(NewFrameOk, size));
    } else if (name == "numbits") {
        std::vector<uint8_t> a(kInputFixed + 4, 0);  // num_bits 65535 in a 36-byte datagram
        InputHead(a.data(), gPeerAck + 1, 65535, 4);
        out.push_back(a);
        std::vector<uint8_t> b(kInputFixed + 512, 0);  // 4096 frames without a change: the sender's own limit is 4095 bits
        InputHead(b.data(), gPeerAck + 1, 4096, 4);
        out.push_back(b);
        std::vector<uint8_t> c(4096, 0);  // a full buffer of them
        InputHead(c.data(), gPeerAck + 1, (4096 - kInputFixed) * 8, 4);
        out.push_back(c);
    } else if (name == "button") {  // button indexes past the input (stock: set() past bits[18])
        out.push_back(WithNewFrame(NewFrameButton200));
        out.push_back(WithNewFrame(NewFrameButton255));
        out.push_back(WithNewFrame(NewFrameButton32));
    } else if (name == "unterminated") {  // the new frame's end bit is missing (stock: udp_proto.cpp:575)
        out.push_back(WithNewFrame(NewFrameRecordOnly));
    } else if (name == "halfrecord") {  // num_bits ends inside a record
        out.push_back(WithNewFrame(NewFrameOk, 4, 5));
        out.push_back(WithNewFrame(NewFrameOk, 4, 9));
    } else if (name == "gap") {  // frames that do not follow the last one (stock: udp_proto.cpp:561)
        std::vector<uint8_t> a(kInputFixed + 1, 0);
        InputHead(a.data(), gMyFrame + 5, 1, 4);
        out.push_back(a);
    } else if (name == "farstart") {
        for (uint32_t s : {0x7FFFFF00u, 0xFFFFFFF0u, 0x80000000u}) {
            std::vector<uint8_t> a(kInputFixed + 1, 0);
            InputHead(a.data(), 0, 1, 4);
            W32(a.data() + 21, s);
            out.push_back(a);
        }
    } else if (name == "status") {  // connect status going back (stock: udp_proto.cpp:536)
        std::vector<uint8_t> a(2048, 0);
        a.resize(ValidInput(a.data()));
        for (int i = 0; i < 4; ++i) W32(a.data() + 5 + 4 * i, (uint32_t)(-1000) << 1);
        out.push_back(a);
    } else if (name == "trunc") {
        std::vector<uint8_t> full(2048, 0);
        full.resize(ValidInput(full.data()));
        for (int len : {0, 1, 3, 4}) out.push_back(std::vector<uint8_t>(full.begin(), full.begin() + len));
        for (int len : {5, 6, 20, 31}) {  // each with its own sequence number
            std::vector<uint8_t> a(full.begin(), full.begin() + len);
            W16(a.data() + 2, gSeq++);
            out.push_back(a);
        }
        std::vector<uint8_t> b(kInputFixed + 3, 0xFF);  // 80 bits announced, 3 bytes there
        InputHead(b.data(), gPeerAck + 1, 80, 4);
        out.push_back(b);
        for (int type : {T_SYNCREQ, T_SYNCREP, T_QREPORT, T_QREPLY, T_INPUTACK}) {
            std::vector<uint8_t> a(6, 0);
            Header(a.data(), type, gMagic);
            out.push_back(a);
        }
    } else if (name == "type") {  // stock: LogMsg's assertion
        for (int type : {0, 8, 100, 255})
            for (int len : {5, 40}) {
                std::vector<uint8_t> a(len, 0);
                Header(a.data(), type, gMagic);
                out.push_back(a);
            }
    } else if (name == "oversize") {  // longer than GGPO's receive buffer
        std::vector<uint8_t> a(5000, 0);
        InputHead(a.data(), gPeerAck + 1, 16, 4);
        out.push_back(a);
        out.push_back(a);
    } else if (name == "random") {  // random bytes behind a valid header; the bits that are protocol commands (disconnect) cleared
        for (int n = 0; n < 60; ++n) {
            const int len = Rnd() % 140;
            std::vector<uint8_t> a(len < kHdr ? kHdr : len);
            for (auto& c : a) c = (uint8_t)Rnd();
            Header(a.data(), Rnd() % 10, gMagic);
            if (a[4] == T_INPUT) {
                for (int o : {5, 9, 13, 17, 25})
                    if ((int)a.size() > o) a[o] &= 0xFE;
            }
            out.push_back(a);
        }
    } else if (name == "bitfuzz") {  // plausible header (size 4, next frame), random bit stream
        for (int n = 0; n < 60; ++n) {
            const int bits = 1 + Rnd() % 300;
            std::vector<uint8_t> a(kInputFixed + (bits + 7) / 8);
            for (auto& c : a) c = (uint8_t)Rnd();
            InputHead(a.data(), gPeerAck + 1 - (int)(Rnd() % 3), bits, 4);
            out.push_back(a);
        }
    } else if (name == "validfuzz") {  // well-formed random streams (1-80 frames, buttons 0-31): taken as inputs, within the limits
        for (int n = 0; n < 20; ++n) {
            std::vector<uint8_t> a(2048, 0);
            BitW w{a.data() + kInputFixed};
            const int frames = 1 + Rnd() % 80;
            for (int f = 0; f < frames; ++f) {
                for (int r = Rnd() % 4; r > 0; --r) w.Record(Rnd() & 1, Rnd() % 32);
                w.Bit(0);
            }
            InputHead(a.data(), gPeerAck + 1 - (int)(Rnd() % 3), w.off, 4);
            a.resize(kInputFixed + (w.off + 7) / 8);
            out.push_back(a);
        }
    } else if (name == "flood") {  // 4000 frames without a change in one packet: well formed, far past any window
        std::vector<uint8_t> a(kInputFixed + 500, 0);
        InputHead(a.data(), gPeerAck + 1, 4000, 4);
        out.push_back(a);
    } else if (name == "seqjump") {
        // One sync request from our address with a foreign magic and a sequence number 20000 ahead (ours does not move).
        // OnSyncRequest ignores it, but stock had stored its sequence number first (sync messages skip the magic and the
        // order check): every real packet after it was "out of order" and the session ended in the disconnect timeout.
        std::vector<uint8_t> a(12, 0);
        W16(a.data(), (uint16_t)(gMagic ^ 0x5555));
        W16(a.data() + 2, (uint16_t)(gSeq + 20000));
        a[4] = T_SYNCREQ;
        W32(a.data() + 5, Rnd() & 0xFFFF);
        out.push_back(a);
    } else if (name == "slowack") {
        // Nothing malformed: for 45 frames this side acknowledges no new frame of the peer (its inputs keep arriving), then
        // goes on. With an input that changes every frame (ggpo_peer churn=1) the peer's unacknowledged frames no longer
        // fit one packet (4095 bits): stock asserted in SendPendingOutput; now it sends the frames that fit and the rest
        // after the acknowledgement. The liveness check starts when the acknowledgements resume (see main).
        gAckFrozen = gPeerLast;
        gAckFreeze = 45;
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("usage: raw_peer <myport> <peerport> <my player 1|2> <seconds> <case[,case...]|all|none> [seed]\n");
        return 2;
    }
    const int myPort = atoi(argv[1]), peerPort = atoi(argv[2]);
    gMe = atoi(argv[3]) - 1;
    gOther = 1 - gMe;
    const double seconds = atof(argv[4]);
    std::string spec = argv[5];
    if (argc > 6) gRng = (uint32_t)atoi(argv[6]) * 2654435761u + 1;
    if (spec == "all") spec = "size,numbits,button,unterminated,halfrecord,gap,farstart,status,trunc,type,oversize,random,bitfuzz,flood,seqjump";
    std::vector<std::string> cases;
    bool presync = false;
    for (size_t i = 0; spec != "none" && i <= spec.size();) {
        const size_t j = spec.find(',', i);
        const std::string c = spec.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (c == "presync") presync = true;
        else if (c == "noack") gNoAck = true;
        else if (c == "churncheck") gChurn = true;
        else if (!c.empty()) cases.push_back(c);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    timeBeginPeriod(1);
    gSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)myPort);
    if (bind(gSock, reinterpret_cast<sockaddr*>(&a), sizeof a)) {
        printf("bind failed %d\n", WSAGetLastError());
        return 1;
    }
    u_long nb = 1;
    ioctlsocket(gSock, FIONBIO, &nb);
    gPeer = a;
    gPeer.sin_port = htons((unsigned short)peerPort);
    gIn.reserve(100000);
    gMyRandom = Rnd() & 0xFFFF;

    if (presync) {  // before any handshake: an input packet with magic 0 carrying frame 0 (stock queued it)
        // Wait, without answering, for the peer's first sync request: it is up and syncing (it asks again 150 ms later).
        for (int n = 0; n < 5000; ++n) {
            char buf[4096];
            if (recvfrom(gSock, buf, sizeof buf, 0, nullptr, nullptr) > 0) break;
            Sleep(2);
        }
        for (int n = 0; n < 3; ++n) {
            uint8_t p[64] = {};
            Header(p, T_INPUT, 0);
            for (int i = 0; i < 4; ++i) W32(p + 5 + 4 * i, 0xFFFFFFFEu);
            W32(p + 25, 0xFFFFFFFEu);
            BitW w{p + kInputFixed};
            w.Record(1, 7);
            w.Bit(0);
            W16(p + 29, (uint16_t)w.off);
            p[31] = 4;
            Send(p, kInputFixed + (w.off + 7) / 8);
        }
        Sleep(60);  // the peer polls them while it is still syncing
        printf("[%7.0f] presync: 3 input packets with magic 0 sent to the syncing peer, before any sync reply\n", NowMs());
    }

    const double t0 = NowMs();
    double nextTick = t0, nextSync = t0, nextCase = 0, checkUntil = 0;
    size_t caseIdx = 0;
    int checkAck = 0, checkLast = 0, failures = 0, passed = 0;
    std::string checking;
    uint32_t mask = 0;
    int hold = 0;
    bool reportedBye = false;
    while (NowMs() - t0 < seconds * 1000.0) {
        Pump();
        const double now = NowMs();
        if (!gSynced && now >= nextSync) {
            uint8_t p[16] = {};
            Header(p, T_SYNCREQ, gMagic);
            W32(p + 5, gMyRandom);
            Send(p, 12);
            nextSync = now + 100;
        }
        if (gPeerBye && !reportedBye) {
            reportedBye = true;
            printf("[%7.0f] the peer sent disconnect_requested (peer frame %d, acked %d of my %d)\n", now, gPeerLast, gPeerAck, gMyFrame - 1);
        }
        if (gSynced && now >= nextTick) {
            nextTick += 1000.0 / 60.0;
            if (gAckFreeze > 0) --gAckFreeze;
            // The peer may have taken frames I never produced (a malformed packet that was well formed after all): adopt them.
            while (gMyFrame <= gPeerAck) {
                gIn.push_back(gIn.empty() ? 0 : gIn.back());
                ++gMyFrame;
            }
            if (gMyFrame <= gPeerLast + 8 || gPeerLast < 0 && gMyFrame < 8) {  // GGPO's prediction barrier, roughly
                if (--hold <= 0) {
                    mask = Rnd();
                    if (Rnd() % 4 == 0) mask = 0;
                    hold = 1 + Rnd() % 6;
                }
                gIn.push_back(mask);
                ++gMyFrame;
            }
            uint8_t p[2048];
            const int len = ValidInput(p);
            Send(p, len);
            if (!nextCase) nextCase = now + 1500;
            // one group of malformed datagrams, then a liveness check on the valid stream
            if (checking.empty() && caseIdx < cases.size() && now >= nextCase && gPeerLast > 30) {
                const Pkts pk = Case(cases[caseIdx]);
                for (const auto& d : pk) Send(d.data(), (int)d.size());
                checking = cases[caseIdx++];
                checkAck = gPeerAck > gMyFrame ? gPeerAck : gMyFrame;
                checkLast = gPeerLast;
                checkUntil = now + 2500;
                printf("[%7.0f] case %-12s: %d datagram(s) sent at my frame %d (peer acked %d, peer frame %d)\n", now, checking.c_str(),
                       (int)pk.size(), gMyFrame - 1, gPeerAck, gPeerLast);
            } else if (!checking.empty()) {
                // alive: the peer acknowledged frames produced after the injection and sent 20 more of its own
                if (gAckFreeze > 0) {  // slowack: counted from the tick the acknowledgements resume
                    checkAck = gPeerAck > gMyFrame ? gPeerAck : gMyFrame;
                    checkLast = gPeerLast;
                    checkUntil = now + 2500;
                } else if (gPeerAck >= checkAck + 5 && gPeerLast >= checkLast + 20) {
                    printf("[%7.0f]      %-12s: peer alive, session kept (acked %d, peer frame %d)\n", now, checking.c_str(), gPeerAck, gPeerLast);
                    ++passed;
                    checking.clear();
                    nextCase = now + 150;
                } else if (now > checkUntil) {
                    printf("[%7.0f]      %-12s: FAILED - the peer stopped (acked %d, peer frame %d, bye %d)\n", now, checking.c_str(), gPeerAck,
                           gPeerLast, gPeerBye ? 1 : 0);
                    ++failures;
                    checking.clear();
                    caseIdx = cases.size();
                }
            }
        }
        Sleep(1);
    }
    if (gChurn && (gMismatch || gGaps))
        printf("churncheck: FAILED - %d of the peer's frames were not Churn(frame) (first: frame %d), stream gaps %d\n", gMismatch,
               gFirstMismatch, gGaps);
    else if (gChurn)
        printf("churncheck: all %d decoded frames of the peer were Churn(frame), no gap\n", gPeerLast + 1);
    printf("raw_peer done: %d datagrams sent, %d received (%d inputs), my frames %d, peer acked %d, peer frame %d, peer bye %d, "
           "stream gaps %d; cases passed %d, failed %d%s\n",
           gSent, gPeerPackets, gPeerInputPackets, gMyFrame, gPeerAck, gPeerLast, gPeerBye ? 1 : 0, gGaps, passed, failures,
           !checking.empty() ? " (one still being checked)" : "");
    return failures || (gChurn && (gMismatch || gGaps)) ? 1 : 0;
}
