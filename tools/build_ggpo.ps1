# Builds GGPO (third_party, unmodified) as a static library for the clang (llvm-mingw i686) DLL build.
# A patched copy of the library sources goes to build\ggpo-src:
#  - udp_proto.h:56 'UdpProtocol::Event(Type t = Unknown)' -> 'Event(...)' (extra qualification only MSVC accepts);
#  - synctest.cpp RaiseSyncError: DebugBreak() (kills the game without a debugger) -> MK9R_GgpoSyncError(buf),
#    implemented by MK9Rollback (logs and counts);
#  - p2p.cpp: lockstep hooks, and AddLocalInput sends the frames a mid-session delay increase padded (below);
#  - rc25 (2026-11-09 GGPO audit): QueryPerformanceCounter clock, uninitialized members, ggpo_no_disconnect_rewind,
#    frame advantage from half the minimum round trip (ggpo_rtt_min_ms), ggpo_after_send, logging decided once
#    (ggpo_log_on), ggpo_bind_loopback and no WSAECONNRESET on GGPO's socket (blocks marked rc25 below);
#  - round 3 (2026-11-10 review): ggpo_bye_on_close, validation of what the peer sends (a malformed packet is dropped
#    instead of closing the game), socket errors (ggpo_bind_failed, no assertion in sendto) (blocks marked round 3 below);
#  - rc30 (2026-11-12 review and sanitizer fuzz): quality replies out of range, sequence numbers of sync messages, Poll's
#    event handle, the running state's timers, zeroed messages, magic numbers and sync randoms from a seeded generator,
#    late sync replies, input packets limited to the frames that fit (block marked rc30 below).
# Output: build\clang\libggpo.a (-Out build\x: build\x\libggpo.a, for a test build: .\build.ps1 -GgpoLib build\x\libggpo.a).
# GGPO is built without -DUNICODE (platform_windows.cpp uses char buffers).
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root 'third_party\ggpo-7ddadef8546a7d99ff0b3530c6056bc8ee4b9c0a\src'
$work = Join-Path $root 'build\ggpo-src'
$out = if ($Out) { Join-Path $root $Out } else { Join-Path $root 'build\clang' }
$bin = Join-Path $root '..\tools\llvm-mingw-20260922-ucrt-x86_64\bin'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work, $out | Out-Null
Copy-Item -Recurse (Join-Path $src 'lib') $work
Copy-Item -Recurse (Join-Path $src 'include') $work
$proto = Join-Path $work 'lib\ggpo\network\udp_proto.h'
$t = [IO.File]::ReadAllText($proto)
if (-not $t.Contains('UdpProtocol::Event(Type t = Unknown)')) { throw 'udp_proto.h not as expected' }
[IO.File]::WriteAllText($proto, $t.Replace('UdpProtocol::Event(Type t = Unknown)', 'Event(Type t = Unknown)'))
$st = Join-Path $work 'lib\ggpo\backends\synctest.cpp'
$t = [IO.File]::ReadAllText($st)
if (-not $t.Contains('   DebugBreak();')) { throw 'synctest.cpp not as expected' }
$t = $t.Replace('   DebugBreak();', '   MK9R_GgpoSyncError(buf);')
$t = $t.Replace('#include "synctest.h"', "#include `"synctest.h`"`nextern `"C`" void MK9R_GgpoSyncError(const char* msg);")
[IO.File]::WriteAllText($st, $t)
# Lockstep mode: MK9Rollback sets ggpo_prediction_frames_override = 1 before ggpo_start_session, so a frame only
# runs once the remote input for it has arrived (Sync::AddLocalInput rejects while _framecount > last confirmed):
# GGPO never predicts and never rolls back. 0 keeps MAX_PREDICTION_FRAMES (rollback).
$p2p = Join-Path $work 'lib\ggpo\backends\p2p.cpp'
$t = [IO.File]::ReadAllText($p2p)
if (-not $t.Contains('   config.num_prediction_frames = MAX_PREDICTION_FRAMES;')) { throw 'p2p.cpp not as expected' }
$t = $t.Replace('   config.num_prediction_frames = MAX_PREDICTION_FRAMES;',
                '   config.num_prediction_frames = ggpo_prediction_frames_override > 0 ? ggpo_prediction_frames_override : MAX_PREDICTION_FRAMES;')
$t = $t.Replace('#include "p2p.h"', "#include `"p2p.h`"`nextern `"C`" int ggpo_prediction_frames_override;`nextern `"C`" { int ggpo_prediction_frames_override = 0; }")
# The prediction barrier alone deadlocks (the confirmed frame is a minimum that includes the local player's own last
# input, which the barrier rejects), so lockstep actually waits in MK9Rollback for ggpo_last_confirmed_frame to reach
# the frame before synchronizing it; the override stays 0.
$old = '            _sync.SetLastConfirmedFrame(total_min_confirmed);'
if (-not $t.Contains($old)) { throw 'p2p.cpp confirmed frame not as expected' }
$t = $t.Replace($old, $old + "`n            ggpo_last_confirmed_frame = total_min_confirmed;")
$t = $t.Replace('extern "C" { int ggpo_prediction_frames_override = 0; }', 'extern "C" { int ggpo_prediction_frames_override = 0; int ggpo_last_confirmed_frame = -1; int ggpo_rollback_safe_frame = -1; }')
# Smoothness (2026-11-04): right before each poll's rollback check, the oldest frame some player's input is still owed for
# (the minimum over connected queues of the last frame received or added). A rollback loads the first mispredicted frame,
# which follows the last received one, and a disconnect rewinds to the peer's last frame: never a frame before this one,
# so MK9Rollback's replays skip saving those (no state copy). -1 before the first poll of a session.
$old = '         _sync.CheckSimulation(timeout);'
if (([regex]::Matches($t, [regex]::Escape($old))).Count -ne 1) { throw 'p2p.cpp CheckSimulation not as expected' }
$t = $t.Replace($old, "         { int mk9r_min = MAX_INT; for (int i = 0; i < _num_players; i++) if (!_local_connect_status[i].disconnected) mk9r_min = MIN(mk9r_min, _local_connect_status[i].last_frame); ggpo_rollback_safe_frame = mk9r_min == MAX_INT ? -1 : mk9r_min; }`n" + $old)
# The input delay may change during a session (the online mode adapts it to the ping between rounds; ggponet.h asks
# for it before the first sync only). A larger delay makes InputQueue::AdvanceQueueHead pad the local queue with copies
# of the last input, but only the new input went to the remote, whose stream is consecutive frames from start_frame
# (UdpProtocol::SendPendingOutput / OnInput): every later frame would have been read one slot off. AddLocalInput now
# sends the padding frames first. A smaller delay drops the local input (frame NullFrame, nothing sent): no gap.
function Set-Once([string]$text, [string]$old, [string]$new, [string]$what) {
    $n = ([regex]::Matches($text, [regex]::Escape($old))).Count
    if ($n -ne 1) { throw "GGPO patch '$what' not as expected ($n matches)" }
    return $text.Replace($old, $new)
}
$t = Set-Once $t 'static const int RECOMMENDATION_INTERVAL           = 240;' ("// MK9Rollback: the last input sent per local queue (one P2P session at a time).`n" +
    "static int mk9r_last_sent_frame[GGPO_MAX_PLAYERS];`nstatic GameInput mk9r_last_sent_input[GGPO_MAX_PLAYERS];`n`n" +
    'static const int RECOMMENDATION_INTERVAL           = 240;') 'statics'
$t = Set-Once $t '   _next_recommended_sleep = 0;' ("   _next_recommended_sleep = 0;`n" +
    "   ggpo_rollback_safe_frame = -1;`n" +
    "   for (int i = 0; i < GGPO_MAX_PLAYERS; i++) mk9r_last_sent_frame[i] = GameInput::NullFrame;") 'constructor'
$old = '      Log("setting local connect status for local queue %d to %d", queue, input.frame);'
$t = Set-Once $t $old ("      // MK9Rollback: the frames the local queue padded after a delay increase, as it padded them.`n" +
    "      if (mk9r_last_sent_frame[queue] != GameInput::NullFrame) {`n" +
    "         for (int f = mk9r_last_sent_frame[queue] + 1; f < input.frame; f++) {`n" +
    "            GameInput pad = mk9r_last_sent_input[queue];`n" +
    "            pad.frame = f;`n" +
    "            _local_connect_status[queue].last_frame = f;`n" +
    "            for (int i = 0; i < _num_players; i++) {`n" +
    "               if (_endpoints[i].IsInitialized()) {`n" +
    "                  _endpoints[i].SendInput(pad);`n" +
    "               }`n" +
    "            }`n" +
    "         }`n" +
    "      }`n" +
    "      mk9r_last_sent_frame[queue] = input.frame;`n" +
    "      mk9r_last_sent_input[queue] = input;`n" + $old) 'AddLocalInput'
# Disconnect rewind only to a frame that is still saved (see sync.cpp HasSavedFrame below): a peer that sent no input
# (last frame -1) asserted in FindSavedFrameIndex and closed the game. MK9Rollback stops the session on the disconnect
# event anyway; the frames already run on predicted input stay.
$t = Set-Once $t '   if (syncto < framecount) {' '   if (syncto < framecount && _sync.HasSavedFrame(syncto)) {' 'DisconnectPlayerQueue rewind guard'
# Disconnect rewind switch (rc25, 2026-11-09 GGPO audit B4): MK9Rollback ends the session at the DISCONNECTED event that
# follows the rewind, so the rewind (a load and up to 8 replayed frames at the disconnect) only costs a hitch, and its
# target (the peer's last frame, which is also the last confirmed one: the sparse fallback cannot go below it) may be an odd
# frame whose save was skipped (HasSavedFrame sees the token, not whether the frame is loadable).
# ggpo_no_disconnect_rewind = 1 skips it; 0 (default) keeps the guarded rewind above.
$t = Set-Once $t 'extern "C" { int ggpo_prediction_frames_override = 0; int ggpo_last_confirmed_frame = -1; int ggpo_rollback_safe_frame = -1; }' (
    'extern "C" { int ggpo_prediction_frames_override = 0; int ggpo_last_confirmed_frame = -1; int ggpo_rollback_safe_frame = -1; int ggpo_no_disconnect_rewind = 0; }') 'p2p.cpp disconnect rewind flag'
$t = Set-Once $t '   if (syncto < framecount && _sync.HasSavedFrame(syncto)) {' (
    '   if (syncto < framecount && _sync.HasSavedFrame(syncto) && !ggpo_no_disconnect_rewind) {  // MK9Rollback (rc25)') 'p2p.cpp disconnect rewind switch'
[IO.File]::WriteAllText($p2p, $t)
# Lockstep waits (ggpo_idle in a loop) before the very first synchronize, so the confirmed frame can run ahead
# while no frame was requested yet; stock DiscardConfirmedFrames then discards unrequested inputs and GetInput(0)
# asserts (input_queue.cpp:134, Steam self-loopback 19:16). Discard nothing until a frame has been requested.
$iq = Join-Path $work 'lib\ggpo\input_queue.cpp'
$t = [IO.File]::ReadAllText($iq)
$old = "   if (_last_frame_requested != GameInput::NullFrame) {`n      frame = MIN(frame, _last_frame_requested);`n   }"
$t = $t.Replace("`r`n", "`n")
if (-not $t.Contains($old)) { throw 'input_queue.cpp not as expected' }
$t = $t.Replace($old, "   if (_last_frame_requested == GameInput::NullFrame) {`n      return;`n   }`n   frame = MIN(frame, _last_frame_requested);")
# Logging guard (rc25, see log.h below): no formatting when GGPO does not log.
$t = Set-Once $t '   offset = sprintf_s(buf, ARRAY_SIZE(buf), "input q%d | ", _id);' (
    "   if (!GgpoLogOn()) {  // MK9Rollback (rc25): no formatting when GGPO does not log`n      return;`n   }`n" +
    '   offset = sprintf_s(buf, ARRAY_SIZE(buf), "input q%d | ", _id);') 'input_queue.cpp Log guard'
[IO.File]::WriteAllText($iq, $t)
# ...including frame 0: the stock barrier only applies from _framecount >= max, so frame 0 would run on a predicted
# remote input (a button held at fight start -> misprediction -> the lockstep session stops).
$sy = Join-Path $work 'lib\ggpo\sync.cpp'
$t = [IO.File]::ReadAllText($sy)
$old = 'if (_framecount >= _max_prediction_frames && frames_behind >= _max_prediction_frames) {'
if (-not $t.Contains($old)) { throw 'sync.cpp not as expected' }
$t = $t.Replace($old, 'if ((_framecount >= _max_prediction_frames || _max_prediction_frames == 1) && frames_behind >= _max_prediction_frames) {')
# Disconnect rewind (2026-11-04, net2 ns30_d2_loss5): a peer that never finished its start handshake (lost sync packets)
# sent no input, so its last frame was -1; at the 3 s disconnect DisconnectPlayerQueue rewound to it, FindSavedFrameIndex
# found no such frame and asserted (sync.cpp:238): the game closed. HasSavedFrame lets the rewind be skipped.
$old = "int`nSync::FindSavedFrameIndex(int frame)"
$t = $t.Replace("`r`n", "`n")
if (-not $t.Contains($old)) { throw 'sync.cpp FindSavedFrameIndex not as expected' }
$t = $t.Replace($old, "bool`nSync::HasSavedFrame(int frame)`n{`n   if (frame < 0) {`n      return false;`n   }`n" +
    "   for (int i = 0; i < ARRAY_SIZE(_savedstate.frames); i++) {`n      if (_savedstate.frames[i].frame == frame && _savedstate.frames[i].buf) {`n" +
    "         return true;`n      }`n   }`n   return false;`n}`n`n" + $old)
# Sparse replay saves (2026-11-06, net9: replay saves were 12-41 ms of a 46-68 ms rollback on a 100 ms link): MK9Rollback
# skips the saves of odd replayed frames and marks them not loadable. A rollback to such a frame loads the newest frame
# before it that MK9Rollback calls loadable (ggpo_frame_loadable), never one before _last_confirmed_frame (the input
# queues keep inputs from there: SetLastConfirmedFrame discards the older ones), and replays the frames between with
# their confirmed inputs. Without the callback (or with nothing loadable found) the target is loaded as before.
$t = $t.Replace('#include "sync.h"', "#include `"sync.h`"`n" +
    'extern "C" { int (*ggpo_frame_loadable)(unsigned char *buf, int len) = 0; int ggpo_fallback_loads = 0, ggpo_fallback_misses = 0; }  // MK9Rollback')
$old = "   Log(`"Catching up\n`");`n   _rollingback = true;`n"
if (-not $t.Contains($old)) { throw 'sync.cpp AdjustSimulation not as expected' }
$t = $t.Replace($old, $old +
    "`n   if (ggpo_frame_loadable && seek_to != _framecount) {  // MK9Rollback: sparse replay saves`n" +
    "      bool found = false;`n" +
    "      for (int f = seek_to; f >= 0 && f >= _last_confirmed_frame && !found; f--) {`n" +
    "         int k = -1;`n" +
    "         for (int i = 0; i < ARRAY_SIZE(_savedstate.frames); i++) {`n" +
    "            if (_savedstate.frames[i].frame == f && _savedstate.frames[i].buf) {`n" +
    "               k = i;`n" +
    "               break;`n" +
    "            }`n" +
    "         }`n" +
    "         if (k < 0) {`n" +
    "            break;`n" +
    "         }`n" +
    "         if (ggpo_frame_loadable(_savedstate.frames[k].buf, _savedstate.frames[k].cbuf)) {`n" +
    "            found = true;`n" +
    "            if (f != seek_to) {`n" +
    "               ggpo_fallback_loads++;`n" +
    "               seek_to = f;`n" +
    "               count = _framecount - seek_to;`n" +
    "            }`n" +
    "         }`n" +
    "      }`n" +
    "      if (!found) {`n" +
    "         ggpo_fallback_misses++;`n" +
    "      }`n" +
    "   }`n")
[IO.File]::WriteAllText($sy, $t)
$sh = Join-Path $work 'lib\ggpo\sync.h'
$t = [IO.File]::ReadAllText($sh)
$old = '   void AdjustSimulation(int seek_to);'
if (-not $t.Contains($old)) { throw 'sync.h not as expected' }
[IO.File]::WriteAllText($sh, $t.Replace($old, $old + "`n   bool HasSavedFrame(int frame);  // MK9Rollback: the disconnect rewind needs its frame"))
# The network simulator ('netsim', ggpo.network.delay) holds sends in a queue that a closed session dropped: a peer
# that stopped at the timeline restart never delivered its last inputs, which a real network already carries, and
# the other game waited for them until GGPO's disconnect (lockstep_ns60, 2026-10-30). Send them on close instead.
# Without the simulator the queue is always empty here (PumpSendQueue sends at once).
$up = Join-Path $work 'lib\ggpo\network\udp_proto.cpp'
$t = [IO.File]::ReadAllText($up).Replace("`r`n", "`n")
$old = "UdpProtocol::~UdpProtocol()`n{`n   ClearSendQueue();`n}"
if (-not $t.Contains($old)) { throw 'udp_proto.cpp destructor not as expected' }
$t = $t.Replace($old, "UdpProtocol::~UdpProtocol()`n{`n" +
    "   while (_udp && !_send_queue.empty()) {`n" +
    "      QueueEntry &entry = _send_queue.front();`n" +
    "      _udp->SendTo((char *)entry.msg, entry.msg->PacketSize(), 0, (struct sockaddr *)&entry.dest_addr, sizeof entry.dest_addr);`n" +
    "      delete entry.msg;`n" +
    "      _send_queue.pop();`n" +
    "   }`n" +
    "   ClearSendQueue();`n}")
# Network simulator additions ('netsim <delay> <oop> <loss> <stall ms> <stall period ms>', two-instance scenario tests):
# ggpo.loss.percent drops that share of the packets as they would leave; for ggpo.spike.ms of every ggpo.spike.every ms
# nothing leaves (a stalled link, the held packets then go out together). Both 0 (unset) in players' games.
$t = Set-Once $t 'static const int MAX_SEQ_DISTANCE = (1 << 15);' ("static const int MAX_SEQ_DISTANCE = (1 << 15);`n" +
    'static int mk9r_loss_percent = 0, mk9r_spike_ms = 0, mk9r_spike_every = 0;  // MK9Rollback netsim additions') 'udp_proto statics'
$t = Set-Once $t '   _oop_percent = Platform::GetConfigInt("ggpo.oop.percent");' ('   _oop_percent = Platform::GetConfigInt("ggpo.oop.percent");' + "`n" +
    '   mk9r_loss_percent = Platform::GetConfigInt("ggpo.loss.percent");' + "`n" +
    '   mk9r_spike_ms = Platform::GetConfigInt("ggpo.spike.ms");' + "`n" +
    '   mk9r_spike_every = Platform::GetConfigInt("ggpo.spike.every");') 'udp_proto netsim config'
$t = Set-Once $t "      QueueEntry &entry = _send_queue.front();`n`n      if (_send_latency) {" ("      QueueEntry &entry = _send_queue.front();`n" +
    "      if (mk9r_spike_ms > 0 && mk9r_spike_every > 0 && (int)(Platform::GetCurrentTimeMS() % (unsigned)mk9r_spike_every) < mk9r_spike_ms) {`n" +
    "         break;`n      }`n`n      if (_send_latency) {") 'udp_proto stall'
$old = '      if (_oop_percent && !_oo_packet.msg && ((rand() % 100) < _oop_percent)) {'
$t = Set-Once $t $old ("      if (mk9r_loss_percent > 0 && (rand() % 100) < mk9r_loss_percent) {`n         delete entry.msg;`n         _send_queue.pop();`n" +
    "         continue;`n      }`n" + $old) 'udp_proto loss'
# Sync kick (2026-11-04): the game that starts its session first sends its first sync request to a port nobody listens
# on yet; GGPO retries after SYNC_FIRST_RETRY_INTERVAL (500 ms), so that game reached 'running' ~0.8 s after the other
# (every two-instance fight start), which meanwhile ran into the prediction limit and then rolled back from frame 8.
# The peer's own sync request proves it listens now: the first one that arrives before any reply re-sends ours at once.
# A reply to the request it replaces still counts (it may have been in flight). One session, one remote: file statics.
$t = Set-Once $t 'static int mk9r_loss_percent = 0, mk9r_spike_ms = 0, mk9r_spike_every = 0;  // MK9Rollback netsim additions' (
    'static int mk9r_loss_percent = 0, mk9r_spike_ms = 0, mk9r_spike_every = 0;  // MK9Rollback netsim additions' + "`n" +
    'static bool mk9r_sync_kicked = false; static int mk9r_sync_prev_random = -1;  // MK9Rollback sync kick') 'udp_proto sync kick statics'
$t = Set-Once $t "      _state.sync.roundtrips_remaining = NUM_SYNC_PACKETS;`n      SendSyncRequest();" (
    "      _state.sync.roundtrips_remaining = NUM_SYNC_PACKETS;`n      mk9r_sync_kicked = false;`n      mk9r_sync_prev_random = -1;`n      SendSyncRequest();") 'udp_proto sync kick reset'
$t = Set-Once $t "   reply->u.sync_reply.random_reply = msg->u.sync_request.random_request;`n   SendMsg(reply);`n   return true;" (
    "   reply->u.sync_reply.random_reply = msg->u.sync_request.random_request;`n   SendMsg(reply);`n" +
    "   if (_current_state == Syncing && _state.sync.roundtrips_remaining == NUM_SYNC_PACKETS && !mk9r_sync_kicked) {`n" +
    "      mk9r_sync_kicked = true;`n      mk9r_sync_prev_random = (int)_state.sync.random;`n      SendSyncRequest();`n   }`n   return true;") 'udp_proto sync kick'
$t = Set-Once $t "   if (msg->u.sync_reply.random_reply != _state.sync.random) {" (
    "   if (msg->u.sync_reply.random_reply != _state.sync.random && (int)msg->u.sync_reply.random_reply != mk9r_sync_prev_random) {") 'udp_proto sync kick reply'
$t = Set-Once $t "   Log(`"Checking sync state (%d round trips remaining).\n`", _state.sync.roundtrips_remaining);" (
    "   mk9r_sync_prev_random = -1;`n   Log(`"Checking sync state (%d round trips remaining).\n`", _state.sync.roundtrips_remaining);") 'udp_proto sync kick clear'
# Quality report interval as a variable (2026-11-04): the report carries this side's frame advantage (the peer's
# remote_frames_behind) and the ping; stock sends it every 1000 ms, so the frame-advantage balancer saw the peer's view
# up to a second late. MK9Rollback sets ggpo_quality_report_interval ('qualityms N'); default 200 ms since net4
# (2026-11-04, with balancecfg 24 4 3: frames >= 34 ms -22% over the network matrix); 1000 is the stock rate.
$t = Set-Once $t 'static const int QUALITY_REPORT_INTERVAL = 1000;' (
    'extern "C" { int ggpo_quality_report_interval = 200; }  // MK9Rollback: ''qualityms N''' + "`n" +
    '#define QUALITY_REPORT_INTERVAL ggpo_quality_report_interval') 'udp_proto quality interval'
# Sync retry (2026-11-04, net1): a sync request or reply lost after the first round trip was retried after
# SYNC_RETRY_INTERVAL = 2000 ms, so one game reached 'running' 2-3 s after the other and that one waited at frame 0
# (ns30_d2_loss5, ns60_d2_jittery: sessions of 35-36 s instead of 33, single waits of 2.1-3.0 s). 400 ms now; a reply to
# the request a retry replaced still counts (as for the sync kick), so a round trip up to ~800 ms still completes.
$t = Set-Once $t 'static const int SYNC_RETRY_INTERVAL = 2000;' (
    'static const int SYNC_RETRY_INTERVAL = 400;  // MK9Rollback: stock 2000 (a lost sync packet froze the start 2 s)') 'udp_proto sync retry interval'
$t = Set-Once $t "         Log(`"No luck syncing after %d ms... Re-queueing sync packet.\n`", next_interval);`n         SendSyncRequest();" (
    "         Log(`"No luck syncing after %d ms... Re-queueing sync packet.\n`", next_interval);`n" +
    "         mk9r_sync_prev_random = (int)_state.sync.random;  // MK9Rollback: its reply may still be in flight`n" +
    "         SendSyncRequest();") 'udp_proto sync retry keeps the previous random'
# Sync retry timing (2026-11-05, net5): a lost sync packet still cost the game that synced first 450-830 ms in the start
# barrier (ns30_d2_loss5 gf 100001 waits 452/462 ms; ns60_d2_jittery 503/826 ms; the normal wait is one round trip).
# The retry timer ran from _last_send_time, which every send resets, the replies to the peer's own sync requests
# included, so while the peer's handshake went on, ours was not retried at all. It now runs from our last sync request,
# and after the first measured round trip it is 2 x RTT + 50 ms (at least 150, at most the stock-derived 500/400 ms);
# each retry doubles the RTT estimate (backoff), so a link whose RTT grew still gets intervals its replies fit in (a
# reply to the request a retry replaced still counts).
$t = Set-Once $t 'static bool mk9r_sync_kicked = false; static int mk9r_sync_prev_random = -1;  // MK9Rollback sync kick' (
    'static bool mk9r_sync_kicked = false; static int mk9r_sync_prev_random = -1;  // MK9Rollback sync kick' + "`n" +
    'static unsigned int mk9r_sync_sent_ms = 0, mk9r_sync_prev_sent_ms = 0; static int mk9r_sync_rtt_ms = -1;  // MK9Rollback sync retry timing') 'udp_proto sync timing statics'
$t = Set-Once $t "      mk9r_sync_kicked = false;`n      mk9r_sync_prev_random = -1;`n      SendSyncRequest();" (
    "      mk9r_sync_kicked = false;`n      mk9r_sync_prev_random = -1;`n" +
    "      mk9r_sync_sent_ms = mk9r_sync_prev_sent_ms = 0;`n      mk9r_sync_rtt_ms = -1;`n      SendSyncRequest();") 'udp_proto sync timing reset'
$t = Set-Once $t '   _state.sync.random = rand() & 0xFFFF;' (
    "   _state.sync.random = rand() & 0xFFFF;`n" +
    "   mk9r_sync_prev_sent_ms = mk9r_sync_sent_ms;  // MK9Rollback: sync retry timing`n" +
    "   mk9r_sync_sent_ms = Platform::GetCurrentTimeMS();") 'udp_proto sync request time'
$t = Set-Once $t "      if (_last_send_time && _last_send_time + next_interval < now) {" (
    "      if (mk9r_sync_rtt_ms >= 0) {  // MK9Rollback: retry after 2 x RTT + 50 ms once a round trip was measured`n" +
    "         int a = 2 * mk9r_sync_rtt_ms + 50;`n" +
    "         if (a < 150) a = 150;`n" +
    "         if (a < (int)next_interval) next_interval = (unsigned int)a;`n" +
    "      }`n" +
    "      if (mk9r_sync_sent_ms && mk9r_sync_sent_ms + next_interval < now) {  // MK9Rollback: from our last request, not any send") 'udp_proto sync retry timer'
$t = Set-Once $t "         mk9r_sync_prev_random = (int)_state.sync.random;  // MK9Rollback: its reply may still be in flight`n         SendSyncRequest();" (
    "         mk9r_sync_prev_random = (int)_state.sync.random;  // MK9Rollback: its reply may still be in flight`n" +
    "         if (mk9r_sync_rtt_ms >= 0) mk9r_sync_rtt_ms = mk9r_sync_rtt_ms < 1000 ? 2 * mk9r_sync_rtt_ms : 2000;  // MK9Rollback: backoff`n" +
    "         SendSyncRequest();") 'udp_proto sync retry backoff'
$t = Set-Once $t "   mk9r_sync_prev_random = -1;`n   Log(`"Checking sync state (%d round trips remaining).\n`", _state.sync.roundtrips_remaining);" (
    "   {  // MK9Rollback: the round trip of the request this reply answers`n" +
    "      const unsigned int sent = msg->u.sync_reply.random_reply == _state.sync.random ? mk9r_sync_sent_ms : mk9r_sync_prev_sent_ms;`n" +
    "      if (sent) mk9r_sync_rtt_ms = (int)(Platform::GetCurrentTimeMS() - sent);`n" +
    "   }`n" +
    "   mk9r_sync_prev_random = -1;`n   Log(`"Checking sync state (%d round trips remaining).\n`", _state.sync.roundtrips_remaining);") 'udp_proto sync rtt'
# Handshake length (2026-11-05, net6): every session start held the fight's first frame for the whole handshake, 5
# round trips (stock): connected -> synchronized took 300-340 ms at ns30, 490-570 at ns60, 720-830 at ns100, and the
# game that synced first then waited for the other's first input. The peers already know each other (OnlineMode's
# hello exchange, the checksum channel) and both run the same build (environment check), so the handshake only has to
# prove that packets get through both ways with this session's magic numbers: 2 round trips (the first measures the
# round trip the retry timer above uses); 1 since 2026-11-06 (below).
# One round trip (2026-11-06, net9): the first reply already proves both directions with this session's numbers; the
# second round only added a round trip to every start, and its loss cost 230-640 ms (two of 25 starts on ns30_d2_loss5
# held frame 0 666 / 697 ms: round 2 lost twice, retried after 2 x RTT + 50 ms and then the doubled timer).
$t = Set-Once $t 'static const int NUM_SYNC_PACKETS = 5;' (
    'static const int NUM_SYNC_PACKETS = 1;  // MK9Rollback: stock 5 (each round trip held the fight''s first frame)') 'udp_proto sync round trips'
# First-round retry 150 ms from the start (2026-11-06, net10 ns30_d2_loss5: both games started 50 ms apart, the second
# one's first request was lost, and neither had seen the other, so both retried after 500 ms: a 548 ms start). The
# request is a few bytes; sending it every 150 ms while the peer is not up yet costs nothing.
$t = Set-Once $t 'static const int SYNC_FIRST_RETRY_INTERVAL = 500;' (
    'static const int SYNC_FIRST_RETRY_INTERVAL = 150;  // MK9Rollback: stock 500 (one lost first request froze the start 0.5 s)') 'udp_proto first sync retry'
# First-round retry once the peer is known to be up (2026-11-06, net7): the 2-round-trip handshake took 17-216 ms, but 3
# of 22 session starts still waited 435-588 ms at frame 0. In two, one game had been waiting (its requests went nowhere:
# the peer was not listening yet) and answered the peer's first request with the kick; that request or its reply was
# lost (loss5 / jittery), and with no round trip measured yet the next try came SYNC_FIRST_RETRY_INTERVAL (500 ms) after
# it, while the peer, already synchronized, held its frame 0 for this game's first input. Once a sync request or reply
# from the peer has arrived, the peer is listening: until a round trip is measured, retry after 150 ms.
$t = Set-Once $t 'static unsigned int mk9r_sync_sent_ms = 0, mk9r_sync_prev_sent_ms = 0; static int mk9r_sync_rtt_ms = -1;  // MK9Rollback sync retry timing' (
    'static unsigned int mk9r_sync_sent_ms = 0, mk9r_sync_prev_sent_ms = 0; static int mk9r_sync_rtt_ms = -1;  // MK9Rollback sync retry timing' + "`n" +
    'static bool mk9r_sync_peer_seen = false;  // MK9Rollback: a sync request or reply from the peer arrived this handshake') 'udp_proto sync peer-seen static'
$t = Set-Once $t "      mk9r_sync_sent_ms = mk9r_sync_prev_sent_ms = 0;`n      mk9r_sync_rtt_ms = -1;" (
    "      mk9r_sync_sent_ms = mk9r_sync_prev_sent_ms = 0;`n      mk9r_sync_rtt_ms = -1;`n      mk9r_sync_peer_seen = false;") 'udp_proto sync peer-seen reset'
$t = Set-Once $t "      if (mk9r_sync_sent_ms && mk9r_sync_sent_ms + next_interval < now) {" (
    "      else if (mk9r_sync_peer_seen && next_interval > 150) next_interval = 150;  // MK9Rollback: the peer is listening`n" +
    "      if (mk9r_sync_sent_ms && mk9r_sync_sent_ms + next_interval < now) {") 'udp_proto sync retry once the peer is seen'
$t = Set-Once $t "   if (_current_state == Syncing && _state.sync.roundtrips_remaining == NUM_SYNC_PACKETS && !mk9r_sync_kicked) {" (
    "   mk9r_sync_peer_seen = true;  // MK9Rollback`n" +
    "   if (_current_state == Syncing && _state.sync.roundtrips_remaining == NUM_SYNC_PACKETS && !mk9r_sync_kicked) {") 'udp_proto sync peer seen (request)'
$t = Set-Once $t "   if (msg->u.sync_reply.random_reply != _state.sync.random && (int)msg->u.sync_reply.random_reply != mk9r_sync_prev_random) {" (
    "   mk9r_sync_peer_seen = true;  // MK9Rollback`n" +
    "   if (msg->u.sync_reply.random_reply != _state.sync.random && (int)msg->u.sync_reply.random_reply != mk9r_sync_prev_random) {") 'udp_proto sync peer seen (reply)'
# Replies to older requests (2026-11-06): a reply counted only if it answered the current or the previous request, so
# with retries every 150 ms (peer seen, or the first round now) a round trip of 300 ms or more never completed the
# handshake: each reply came back two requests late. The last 8 requests (random, send time) are kept; a reply to any
# of them counts, and the round trip is measured from that request.
$t = Set-Once $t 'static bool mk9r_sync_peer_seen = false;  // MK9Rollback: a sync request or reply from the peer arrived this handshake' (
    'static bool mk9r_sync_peer_seen = false;  // MK9Rollback: a sync request or reply from the peer arrived this handshake' + "`n" +
    'static int mk9r_sync_ring_random[8]; static unsigned int mk9r_sync_ring_ms[8]; static int mk9r_sync_ring_n = 0;  // MK9Rollback: recent sync requests') 'udp_proto sync ring statics'
$t = Set-Once $t "      mk9r_sync_rtt_ms = -1;`n      mk9r_sync_peer_seen = false;" (
    "      mk9r_sync_rtt_ms = -1;`n      mk9r_sync_peer_seen = false;`n      mk9r_sync_ring_n = 0;") 'udp_proto sync ring reset'
$t = Set-Once $t "   mk9r_sync_sent_ms = Platform::GetCurrentTimeMS();`n" (
    "   mk9r_sync_sent_ms = Platform::GetCurrentTimeMS();`n" +
    "   mk9r_sync_ring_random[mk9r_sync_ring_n % 8] = (int)_state.sync.random;  // MK9Rollback: recent sync requests`n" +
    "   mk9r_sync_ring_ms[mk9r_sync_ring_n % 8] = mk9r_sync_sent_ms;`n" +
    "   mk9r_sync_ring_n++;`n") 'udp_proto sync ring record'
$t = Set-Once $t "   if (msg->u.sync_reply.random_reply != _state.sync.random && (int)msg->u.sync_reply.random_reply != mk9r_sync_prev_random) {" (
    "   int mk9r_ring_hit = -1;  // MK9Rollback: a reply to any of the last 8 requests counts`n" +
    "   for (int j = 0; j < 8 && j < mk9r_sync_ring_n; j++) {`n" +
    "      const int r = (mk9r_sync_ring_n - 1 - j) % 8;`n" +
    "      if (mk9r_sync_ring_random[r] == (int)msg->u.sync_reply.random_reply) {`n" +
    "         mk9r_ring_hit = r;`n" +
    "         break;`n" +
    "      }`n" +
    "   }`n" +
    "   if (msg->u.sync_reply.random_reply != _state.sync.random && (int)msg->u.sync_reply.random_reply != mk9r_sync_prev_random && mk9r_ring_hit < 0) {") 'udp_proto sync ring accept'
$t = Set-Once $t "      const unsigned int sent = msg->u.sync_reply.random_reply == _state.sync.random ? mk9r_sync_sent_ms : mk9r_sync_prev_sent_ms;" (
    "      const unsigned int sent = mk9r_ring_hit >= 0 ? mk9r_sync_ring_ms[mk9r_ring_hit] : (msg->u.sync_reply.random_reply == _state.sync.random ? mk9r_sync_sent_ms : mk9r_sync_prev_sent_ms);") 'udp_proto sync ring rtt'
# Running retry (2026-11-08, the field session's network freezes): a game blocked at the prediction limit adds no input,
# so its unacknowledged inputs only went out again when no new input frame had arrived for RUNNING_RETRY_INTERVAL (200 ms,
# stock), counted from the last new frame. After a short outage of one direction (the friend's uplink: their waits were
# ours minus 8 frames of prediction) the first resend that could get through came on that 200 ms grid, so the freezes
# came in steps (ours 582-587 ms x7, ~647 x2, ~774 x2 in 12) instead of following the outage. Now a variable
# ('netretry N', MK9Rollback default 34 ms: two frames); in normal play a new frame arrives every 16.7 ms, so it only
# fires when inputs stop (or arrive more than 34 ms apart: one extra input packet of a few bytes).
$t = Set-Once $t 'static const int RUNNING_RETRY_INTERVAL = 200;' (
    'extern "C" { int ggpo_running_retry_interval = 34; }  // MK9Rollback: ''netretry N'' (stock 200)' + "`n" +
    '#define RUNNING_RETRY_INTERVAL ggpo_running_retry_interval') 'udp_proto running retry interval'
# Netsim drop mode (2026-11-08): 'netsim ... <stall ms> <period ms> 1' drops the packets of the stall window instead of
# holding them (a dead link loses them; held packets all arrive at the window's end and recover without any resend).
$t = Set-Once $t 'static bool mk9r_sync_kicked = false; static int mk9r_sync_prev_random = -1;  // MK9Rollback sync kick' (
    'static bool mk9r_sync_kicked = false; static int mk9r_sync_prev_random = -1;  // MK9Rollback sync kick' + "`n" +
    'static int mk9r_spike_drop = 0;  // MK9Rollback netsim: the stall window drops packets') 'udp_proto spike drop static'
$t = Set-Once $t '   mk9r_spike_every = Platform::GetConfigInt("ggpo.spike.every");' (
    '   mk9r_spike_every = Platform::GetConfigInt("ggpo.spike.every");' + "`n" +
    '   mk9r_spike_drop = Platform::GetConfigInt("ggpo.spike.drop");') 'udp_proto spike drop config'
$t = Set-Once $t "      if (mk9r_spike_ms > 0 && mk9r_spike_every > 0 && (int)(Platform::GetCurrentTimeMS() % (unsigned)mk9r_spike_every) < mk9r_spike_ms) {`n         break;`n      }" (
    "      if (mk9r_spike_ms > 0 && mk9r_spike_every > 0 && (int)(Platform::GetCurrentTimeMS() % (unsigned)mk9r_spike_every) < mk9r_spike_ms) {`n" +
    "         if (mk9r_spike_drop) {  // MK9Rollback: a dead link loses the packets`n" +
    "            delete entry.msg;`n" +
    "            _send_queue.pop();`n" +
    "            continue;`n" +
    "         }`n" +
    "         break;`n      }") 'udp_proto spike drop'
# rc25 (2026-11-09 GGPO audit).
# B3: members stock left uninitialized (ping was heap garbage until the first quality reply, and the first quality report,
# sent as the session starts running, carried a frame advantage computed from it), and the first network stats update
# divided by zero (kbps INT_MIN for a second).
$t = Set-Once $t '   _next_recv_seq(0),' ("   _next_recv_seq(0),`n" +
    "   _round_trip_time(0),  // MK9Rollback (rc25): these four were uninitialized`n" +
    "   _kbps_sent(0),`n" +
    "   _last_recv_time(0),`n" +
    "   _current_state(Syncing),") 'udp_proto.cpp member init'
$t = Set-Once $t '   float seconds = (float)((now - _stats_start_time) / 1000.0);' (
    '   float seconds = (float)((now - _stats_start_time) / 1000.0);' + "`n" +
    "   if (seconds <= 0) {  // MK9Rollback (rc25): the first call divided by zero`n      return;`n   }") 'udp_proto.cpp stats first call'
# I1: the frame advantage (this game's 'frames behind', sent to the peer in each quality report; MK9Rollback's balance
# reads remote - local) estimated the peer's frame as its last input + the whole latest round trip in frames, truncated.
# The symmetric part cancels in remote - local, but each side's single sample differs: round trips read in whole frames
# (the field link's ~22 ms as 33 or 50 ms: 1 or 3 frames), and the peer's reply waits behind its rollback and live save
# before the bridge forwards it. Now half the minimum of the last 5 round trips (1 s at 'qualityms 200'), rounded (GGRS
# also uses the one-way trip), and 0 until an input and a round trip are in. ggpo_rtt_min_ms exports that minimum (-1
# before the first sample; reset for each session and handshake). ns.network.ping stays the latest sample.
$t = Set-Once $t 'static int mk9r_sync_ring_random[8]; static unsigned int mk9r_sync_ring_ms[8]; static int mk9r_sync_ring_n = 0;  // MK9Rollback: recent sync requests' (
    'static int mk9r_sync_ring_random[8]; static unsigned int mk9r_sync_ring_ms[8]; static int mk9r_sync_ring_n = 0;  // MK9Rollback: recent sync requests' + "`n" +
    'static int mk9r_rtt_ring[5]; static int mk9r_rtt_n = 0;  // MK9Rollback (rc25): the last 5 quality-report round trips' + "`n" +
    'extern "C" { int ggpo_rtt_min_ms = -1; }  // MK9Rollback (rc25): their minimum in ms, -1 before the first') 'udp_proto.cpp rtt statics'
$t = Set-Once $t '   mk9r_spike_drop = Platform::GetConfigInt("ggpo.spike.drop");' (
    '   mk9r_spike_drop = Platform::GetConfigInt("ggpo.spike.drop");' + "`n" +
    "   mk9r_rtt_n = 0;  // MK9Rollback (rc25): a new session's endpoints`n   ggpo_rtt_min_ms = -1;") 'udp_proto.cpp rtt reset (session)'
$t = Set-Once $t "      mk9r_sync_ring_n = 0;" ("      mk9r_sync_ring_n = 0;`n      mk9r_rtt_n = 0;  // MK9Rollback (rc25)`n      ggpo_rtt_min_ms = -1;") 'udp_proto.cpp rtt reset (handshake)'
$t = Set-Once $t '   _round_trip_time = Platform::GetCurrentTimeMS() - msg->u.quality_reply.pong;' (
    '   _round_trip_time = Platform::GetCurrentTimeMS() - msg->u.quality_reply.pong;' + "`n" +
    "   if (_round_trip_time >= 0) {  // MK9Rollback (rc25): the minimum of the last 5`n" +
    "      mk9r_rtt_ring[mk9r_rtt_n % 5] = _round_trip_time;`n" +
    "      mk9r_rtt_n++;`n" +
    "      int m = mk9r_rtt_ring[0];`n" +
    "      for (int i = 1; i < 5 && i < mk9r_rtt_n; i++) {`n" +
    "         m = MIN(m, mk9r_rtt_ring[i]);`n" +
    "      }`n" +
    "      ggpo_rtt_min_ms = m;`n" +
    "   }") 'udp_proto.cpp rtt min'
$t = Set-Once $t '   int remoteFrame = _last_received_input.frame + (_round_trip_time * 60 / 1000);' (
    "   if (_last_received_input.frame < 0 || ggpo_rtt_min_ms < 0) {  // MK9Rollback (rc25): nothing to estimate from yet`n" +
    "      _local_frame_advantage = 0;`n" +
    "      return;`n" +
    "   }`n" +
    "   // MK9Rollback (rc25): the one-way trip (half the minimum round trip), rounded; stock: the latest whole round trip, truncated`n" +
    "   int remoteFrame = _last_received_input.frame + (ggpo_rtt_min_ms * 60 + 1000) / 2000;") 'udp_proto.cpp frame advantage'
# I3: logging guard (see log.h below): no formatting, and no description of each received input, when GGPO does not log.
$t = Set-Once $t '   sprintf_s(buf, ARRAY_SIZE(buf), "udpproto%d | ", _queue);' (
    "   if (!GgpoLogOn()) {  // MK9Rollback (rc25): no formatting when GGPO does not log`n      return;`n   }`n" +
    '   sprintf_s(buf, ARRAY_SIZE(buf), "udpproto%d | ", _queue);') 'udp_proto.cpp Log guard'
$t = Set-Once $t '            _last_received_input.desc(desc, ARRAY_SIZE(desc));' (
    "            desc[0] = 0;  // MK9Rollback (rc25): described for the log only`n" +
    "            if (GgpoLogOn()) {`n" +
    "               _last_received_input.desc(desc, ARRAY_SIZE(desc));`n" +
    "            }") 'udp_proto.cpp desc guard'
# B2 (platform_windows.h below) moves GGPO's clock to QueryPerformanceCounter counted from the process's first use: the
# netsim stall windows keep timeGetTime, the system-wide clock, so two test instances on one PC still share their phase.
$t = Set-Once $t '(int)(Platform::GetCurrentTimeMS() % (unsigned)mk9r_spike_every)' '(int)(timeGetTime() % (unsigned)mk9r_spike_every)' 'udp_proto.cpp netsim stall phase'
[IO.File]::WriteAllText($up, $t)
# A failed GGPO assertion showed a message box (behind the fullscreen game: a frozen screen until someone found and
# dismissed it) before exit(0). MK9R_GgpoAssert logs it and closes the game at once.
$pw = Join-Path $work 'lib\ggpo\platform_windows.h'
$t = [IO.File]::ReadAllText($pw)
$old = '   static void AssertFailed(char *msg) { MessageBoxA(NULL, msg, "GGPO Assertion Failed", MB_OK | MB_ICONEXCLAMATION); }'
if (-not $t.Contains($old)) { throw 'platform_windows.h not as expected' }
$t = $t.Replace($old, '   static void AssertFailed(char *msg) { MK9R_GgpoAssert(msg); }')
$t = $t.Replace('class Platform {', "extern `"C`" void MK9R_GgpoAssert(const char* msg);`n`nclass Platform {")
# GGPO's clock (rc25, 2026-11-09 GGPO audit B2): timeGetTime wraps every 49.7 days of uptime (Fast Startup keeps the
# uptime), and every 't + interval < now' timer then fired at once: a disconnect 3 s before the wrap. It also ticks at the
# system timer rate (15.6 ms unless some process raises it: 'netretry 34' fired at ~47 ms). Now milliseconds from
# QueryPerformanceCounter since the clock's first use, plus 1000 (0 means 'never' to several timers). Only differences of
# this process's own readings are used (the quality report's ping comes back to its sender), so the base is free.
$t = Set-Once $t '   static uint32 GetCurrentTimeMS() { return timeGetTime(); }' (
    "   static uint32 GetCurrentTimeMS() {  // MK9Rollback (rc25): QueryPerformanceCounter, see build_ggpo.ps1`n" +
    "      static LARGE_INTEGER f = {}, t0 = {};`n" +
    "      LARGE_INTEGER t;`n" +
    "      if (!f.QuadPart) {`n" +
    "         QueryPerformanceCounter(&t0);`n" +
    "         QueryPerformanceFrequency(&f);`n" +
    "      }`n" +
    "      QueryPerformanceCounter(&t);`n" +
    "      return 1000u + (uint32)((t.QuadPart - t0.QuadPart) * 1000 / f.QuadPart);`n" +
    "   }") 'platform_windows.h QPC clock'
[IO.File]::WriteAllText($pw, $t)
# Logging guard (rc25, 2026-11-09 GGPO audit I3): with logging off, every Log call still read two environment variables,
# and the UdpProtocol / InputQueue / Udp wrappers formatted their text first (plus each received input's description and
# inet_ntop for each packet). Decided once now: ggpo_log_on -1 undecided, 0 off, 1 on, from ggpo.log / ggpo.log.ignore as
# they are at the first log call; with ggpo.log set, GGPO logs as before.
$lh = Join-Path $work 'lib\ggpo\log.h'
$t = [IO.File]::ReadAllText($lh).Replace("`r`n", "`n")
$t = Set-Once $t 'extern void LogFlushOnLog(bool flush);' ('extern void LogFlushOnLog(bool flush);' + "`n" +
    'extern "C" int ggpo_log_on;  // MK9Rollback (rc25): does GGPO log? -1 undecided, 0 no, 1 yes' + "`n" +
    'void GgpoLogDecide();' + "`n" +
    'inline bool GgpoLogOn() { if (ggpo_log_on < 0) GgpoLogDecide(); return ggpo_log_on != 0; }') 'log.h guard'
[IO.File]::WriteAllText($lh, $t)
$lc = Join-Path $work 'lib\ggpo\log.cpp'
$t = [IO.File]::ReadAllText($lc).Replace("`r`n", "`n")
$t = Set-Once $t 'static FILE *logfile = NULL;' ('static FILE *logfile = NULL;' + "`n" +
    'extern "C" { int ggpo_log_on = -1; }  // MK9Rollback (rc25), see log.h' + "`n" +
    "void GgpoLogDecide()`n{`n" +
    '   ggpo_log_on = (Platform::GetConfigBool("ggpo.log") && !Platform::GetConfigBool("ggpo.log.ignore")) ? 1 : 0;' + "`n}") 'log.cpp decide'
$t = Set-Once $t '   if (!Platform::GetConfigBool("ggpo.log") || Platform::GetConfigBool("ggpo.log.ignore")) {' (
    '   if (!GgpoLogOn()) {  // MK9Rollback (rc25): stock read the environment twice per call') 'log.cpp guard'
[IO.File]::WriteAllText($lc, $t)
# Sockets (rc25, 2026-11-09 GGPO audit I2, I4).
# - ggpo_after_send (default null) runs after every sendto, whatever its result (the error code is kept for the check that
#   follows): MK9Rollback's Steam bridge forwards the datagram at once instead of at its next pump, so the quality replies,
#   resends and keep-alives sent inside ggpo_idle no longer wait behind the rollback and the live save.
# - ggpo_bind_loopback = 1 binds 127.0.0.1 instead of every interface (a bridged session's only peer is the local bridge).
# - SIO_UDP_CONNRESET off (failure ignored): on Windows a datagram sent to a port nobody has bound yet makes the next
#   recvfrom fail with WSAECONNRESET; the receive loop ended on it and left the queued datagrams for the next poll. It now
#   goes on past such an error (at most 63 a poll).
$ud = Join-Path $work 'lib\ggpo\network\udp.cpp'
$t = [IO.File]::ReadAllText($ud).Replace("`r`n", "`n")
$t = Set-Once $t "#include `"udp.h`"`n" ("#include `"udp.h`"`n" +
    "`n#ifndef SIO_UDP_CONNRESET`n#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)`n#endif`n" +
    "// MK9Rollback (rc25): see build_ggpo.ps1 (sockets)`n" +
    "extern `"C`" { void (*ggpo_after_send)(void) = 0; int ggpo_bind_loopback = 0; }`n") 'udp.cpp statics'
$t = Set-Once $t '   sin.sin_addr.s_addr = htonl(INADDR_ANY);' (
    '   sin.sin_addr.s_addr = htonl(ggpo_bind_loopback ? INADDR_LOOPBACK : INADDR_ANY);  // MK9Rollback (rc25)') 'udp.cpp loopback bind'
$t = Set-Once $t '   ioctlsocket(s, FIONBIO, &iMode);' ('   ioctlsocket(s, FIONBIO, &iMode);' + "`n" +
    "   {  // MK9Rollback (rc25): no WSAECONNRESET after an ICMP port-unreachable; failure to set it is ignored`n" +
    "      BOOL off = FALSE;`n" +
    "      DWORD got = 0;`n" +
    "      WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);`n" +
    "   }") 'udp.cpp connreset off'
$t = Set-Once $t '   int res = sendto(_socket, buffer, len, flags, dst, destlen);' ('   int res = sendto(_socket, buffer, len, flags, dst, destlen);' + "`n" +
    "   if (ggpo_after_send) {  // MK9Rollback (rc25): whatever the result; the error code is kept for the check below`n" +
    "      const int mk9r_err = WSAGetLastError();`n" +
    "      ggpo_after_send();`n" +
    "      WSASetLastError(mk9r_err);`n" +
    "   }") 'udp.cpp after send'
$t = Set-Once $t '   Log("sent packet length' '   if (GgpoLogOn()) Log("sent packet length' 'udp.cpp send log guard'
$t = Set-Once $t '         Log("recvfrom returned (len' '         if (GgpoLogOn()) Log("recvfrom returned (len' 'udp.cpp recv log guard'
$t = Set-Once $t '   strcpy_s(buf, "udp | ");' ("   if (!GgpoLogOn()) {  // MK9Rollback (rc25): no formatting when GGPO does not log`n      return;`n   }`n" +
    '   strcpy_s(buf, "udp | ");') 'udp.cpp Log guard'
$t = Set-Once $t "   for (;;) {`n      recv_addr_len = sizeof(recv_addr);" (
    "   int mk9r_resets = 0;  // MK9Rollback (rc25)`n   for (;;) {`n      recv_addr_len = sizeof(recv_addr);") 'udp.cpp recv reset count'
$t = Set-Once $t "         break;`n      } else if (len > 0) {" (
    "         if (error == WSAECONNRESET && ++mk9r_resets < 64) {  // MK9Rollback (rc25): a stale ICMP error; datagrams may follow`n" +
    "            continue;`n" +
    "         }`n" +
    "         break;`n      } else if (len > 0) {") 'udp.cpp recv connreset continue'
[IO.File]::WriteAllText($ud, $t)
# Round 3 (2026-11-10 review): three patches, one block each; they reopen the copies patched above.
#
# Round 3, 1: bye on close. A game that stopped its session just went silent: the other game ran into the prediction limit
# and stood there until its disconnect timeout (3 s in MK9Rollback). With ggpo_bye_on_close = 1 (default 0: the close is
# exactly as before) at ggpo_close_session of a P2P session, every remote endpoint (spectators too) sends three input
# packets with disconnect_requested set before the socket closes. The flag is stock protocol: UdpProtocol::OnInput raises
# the Disconnected event at once, and SendPendingOutput already sets it for an endpoint in the Disconnected state, so a
# peer on an older build understands it. UdpProtocol::SendBye sends what the network simulator still holds first, in
# order (as the destructor's block above), then the three packets through the usual SendMsg (sequence numbers, pending
# inputs, netsim loss), then whatever the simulator kept of those: every datagram leaves through Udp::SendTo
# (ggpo_after_send each) before ~Udp closes the socket. An endpoint still syncing has nothing pending and sends them empty
# (a peer that has not finished its own handshake does not know this magic number yet and ignores them); a disconnected
# one sends them as it would with the next local input; one without a socket (the local player's, or 5 s after its
# disconnect) sends nothing. The flag is read at the close only, so MK9Rollback can set it per stop reason.
$t = [IO.File]::ReadAllText($p2p)
$t = Set-Once $t 'int ggpo_no_disconnect_rewind = 0; }' 'int ggpo_no_disconnect_rewind = 0; int ggpo_bye_on_close = 0; }' 'round 3 bye: p2p.cpp flag'
$t = Set-Once $t '   delete [] _endpoints;' (
    "   if (ggpo_bye_on_close) {  // MK9Rollback (round 3): tell every remote endpoint that this session is over`n" +
    "      for (int i = 0; i < _num_players; i++) {`n" +
    "         _endpoints[i].SendBye();`n" +
    "      }`n" +
    "      for (int i = 0; i < _num_spectators; i++) {`n" +
    "         _spectators[i].SendBye();`n" +
    "      }`n" +
    "   }`n" +
    "   delete [] _endpoints;") 'round 3 bye: p2p.cpp destructor'
[IO.File]::WriteAllText($p2p, $t)
$t = [IO.File]::ReadAllText($proto)
$t = Set-Once $t '   void Disconnect();' ("   void Disconnect();`n   void SendBye();  // MK9Rollback (round 3): ggpo_bye_on_close") 'round 3 bye: udp_proto.h'
[IO.File]::WriteAllText($proto, $t)
$t = [IO.File]::ReadAllText($up)
$t = Set-Once $t 'static const int UDP_SHUTDOWN_TIMER = 5000;' ('static const int UDP_SHUTDOWN_TIMER = 5000;' + "`n" +
    'static bool mk9r_bye = false;  // MK9Rollback (round 3): SendBye is sending (SendPendingOutput sets disconnect_requested)') 'round 3 bye: static'
$t = Set-Once $t '   msg->u.input.disconnect_requested = _current_state == Disconnected;' (
    '   msg->u.input.disconnect_requested = _current_state == Disconnected || mk9r_bye;  // MK9Rollback (round 3)') 'round 3 bye: flag in the packet'
$old = "UdpProtocol::Disconnect()`n{`n   _current_state = Disconnected;`n   _shutdown_timeout = Platform::GetCurrentTimeMS() + UDP_SHUTDOWN_TIMER;`n}`n"
$t = Set-Once $t $old ($old +
    "`n// MK9Rollback (round 3): ggpo_bye_on_close (see build_ggpo.ps1). The queue is emptied before the three packets too, so`n" +
    "// their push never meets a full ring.`n" +
    "void`nUdpProtocol::SendBye()`n{`n" +
    "   if (!_udp) {`n" +
    "      return;`n" +
    "   }`n" +
    "   for (int pass = 0; pass < 2; pass++) {`n" +
    "      while (!_send_queue.empty()) {`n" +
    "         QueueEntry &entry = _send_queue.front();`n" +
    "         _udp->SendTo((char *)entry.msg, entry.msg->PacketSize(), 0, (struct sockaddr *)&entry.dest_addr, sizeof entry.dest_addr);`n" +
    "         delete entry.msg;`n" +
    "         _send_queue.pop();`n" +
    "      }`n" +
    "      if (pass == 0) {`n" +
    "         Log(`"Closing: sending disconnect requests.\n`");`n" +
    "         mk9r_bye = true;`n" +
    "         for (int i = 0; i < 3; i++) {`n" +
    "            SendPendingOutput();`n" +
    "         }`n" +
    "         mk9r_bye = false;`n" +
    "      }`n" +
    "   }`n" +
    "}`n") 'round 3 bye: SendBye'
[IO.File]::WriteAllText($up, $t)
# Round 3, 2: validation of what the peer sends. Every byte of a datagram comes from the peer (over the Steam bridge: from
# whoever writes to the bridge), and stock GGPO trusted all of it: each case below ended in an assertion (MK9R_GgpoAssert:
# the game closes) or in a read or write out of bounds. Such a packet is now dropped with a GGPO log line and the session
# goes on; what a GGPO peer really sends is decoded exactly as before (the checks run before the stock loop, which keeps
# its order and state).
#  a. UdpProtocol::OnMsg had no length check: a short datagram was parsed with the previous datagrams' bytes still in the
#     receive buffer, and an input's num_bits (up to 65535) read up to 8 KB past it. A datagram must hold its message.
#  b. A message type outside 1-7 asserted, in LogMsg (logging on or off) before OnInvalid could: dropped; neither asserts.
#  c. Until the handshake is done the peer's magic number is unknown (0), and a packet with magic 0 passed the filter: its
#     inputs were queued, and the real stream then asserted in p2p.cpp (frames not consecutive). Only sync messages before.
#  d. OnInput, checked before anything changes (line numbers: the stock files): num_bits below MAX_COMPRESSED_BITS (the
#     sender's own limit); input_size 1-18 and, for a player's endpoint, the session's (Peer2PeerBackend passes it:
#     another size asserted in input_queue.cpp:221, and with logging desc() overran its buffer); start_frame not past
#     the next frame (asserted at udp_proto.cpp:561), and a first packet's from 0 to the input window below (a negative
#     one asserted at :612, a far one padded the input queue past its 128 slots: input_queue.cpp:265); the bit stream
#     walked once without effect: every read inside num_bits (asserted at :575), every frame ended by its 0 bit, every
#     button index inside input_size bytes (stock set()/clear()ed any index up to 255 in the 18 bytes of bits[]: a
#     write up to 14 bytes past the array, silent). The write itself checks the index again.
#  e. OnInput: a peer connect status whose last_frame went back asserted (:536); stock already keeps the higher one:
#     logged only, the rest of the packet is used.
#  f. OnInput: frames more than 64 ahead of the local frame (a GGPO peer stops 8 frames past our input: its prediction
#     barrier) overflowed the input queue (128 slots, input_queue.cpp:265), and more than 63 events in one poll the
#     event queue (ring_buffer.h:39). Frames beyond either limit are left for the peer's resend (our ack covers only
#     the frames taken); a GGPO peer never reaches them.
#  g. SendInput: a peer that acknowledges nothing filled _pending_output (63 inputs, about a second) and the push
#     asserted; a full queue now raises the Disconnected event instead (the stock comment's "disconnect them").
#  h. Udp::OnLoopPoll: a datagram longer than the receive buffer (WSAEMSGSIZE) ended that poll's receive loop with
#     datagrams still queued (no assertion, a frame of delay): skipped like a WSAECONNRESET (rc25).
$t = [IO.File]::ReadAllText($proto)
$t = Set-Once $t '   void SetDisconnectNotifyStart(int timeout);' ("   void SetDisconnectNotifyStart(int timeout);`n" +
    "   void SetInputSize(int size) { _mk9r_input_size = size; }  // MK9Rollback (round 3): the size a remote player's input must have") 'round 3 validation: udp_proto.h setter'
$t = Set-Once $t '   RingBuffer<UdpProtocol::Event, 64>  _event_queue;' ("   RingBuffer<UdpProtocol::Event, 64>  _event_queue;`n" +
    "   int _mk9r_input_size;   // MK9Rollback (round 3): 0: not known (only the limits of GameInput apply)`n" +
    "   int _mk9r_local_frame;  // MK9Rollback (round 3): the last SetLocalFrameNumber") 'round 3 validation: udp_proto.h members'
[IO.File]::WriteAllText($proto, $t)
$t = [IO.File]::ReadAllText($p2p)
$t = Set-Once $t '   _endpoints[queue].Init(&_udp, _poll, queue, ip, port, _local_connect_status);' (
    "   _endpoints[queue].Init(&_udp, _poll, queue, ip, port, _local_connect_status);`n" +
    "   _endpoints[queue].SetInputSize(_input_size);  // MK9Rollback (round 3): input packets of another size are dropped") 'round 3 validation: p2p.cpp input size'
[IO.File]::WriteAllText($p2p, $t)
$t = [IO.File]::ReadAllText($up)
$t = Set-Once $t 'static const int UDP_SHUTDOWN_TIMER = 5000;' ('static const int UDP_SHUTDOWN_TIMER = 5000;' + "`n" +
    "// MK9Rollback (round 3): limits on what a remote may send (see build_ggpo.ps1, validation)`n" +
    "static const int MK9R_FRAME_WINDOW = 64;  // input frames taken ahead of the local frame (the input queue holds 128)`n" +
    "static const int MK9R_EVENT_ROOM = 56;    // queued events from which no more input frames are taken (the ring holds 63)`n" +
    "static const int MK9R_PENDING_MAX = 63;   // unacknowledged inputs _pending_output holds (RingBuffer<GameInput, 64>)") 'round 3 validation: limits'
$t = Set-Once $t '   _last_sent_input.init(-1, NULL, 1);' (
    "   _mk9r_input_size = 0;  // MK9Rollback (round 3)`n   _mk9r_local_frame = 0;`n" +
    '   _last_sent_input.init(-1, NULL, 1);') 'round 3 validation: member init'
$t = Set-Once $t "UdpProtocol::SetLocalFrameNumber(int localFrame)`n{`n" (
    "UdpProtocol::SetLocalFrameNumber(int localFrame)`n{`n" +
    "   _mk9r_local_frame = localFrame;  // MK9Rollback (round 3): the input window is counted from it`n") 'round 3 validation: local frame'
# g: the pending output queue.
$t = Set-Once $t '         _pending_output.push(input);' (
    "         if (_pending_output.size() >= MK9R_PENDING_MAX) {  // MK9Rollback (round 3): stock pushed and the ring asserted`n" +
    "            if (!_disconnect_event_sent) {`n" +
    "               Log(`"The peer has acknowledged none of the last %d inputs.  Disconnecting.\n`", _pending_output.size());`n" +
    "               QueueEvent(Event(Event::Disconnected));`n" +
    "               _disconnect_event_sent = true;`n" +
    "            }`n" +
    "            return;`n" +
    "         }`n" +
    '         _pending_output.push(input);') 'round 3 validation: pending output full'
# a, b: the datagram holds its message; c: nothing but the handshake before it is done.
$old = "   // filter out messages that don't match what we expect`n   uint16 seq = msg->hdr.sequence_number;"
$t = Set-Once $t $old (
    "   {  // MK9Rollback (round 3): the datagram must hold its whole message; a type GGPO does not have is dropped`n" +
    "      int need = (int)sizeof(msg->hdr);`n" +
    "      if (len >= need) {`n" +
    "         switch (msg->hdr.type) {`n" +
    "         case UdpMsg::SyncRequest:   need += (int)sizeof(msg->u.sync_request); break;`n" +
    "         case UdpMsg::SyncReply:     need += (int)sizeof(msg->u.sync_reply); break;`n" +
    "         case UdpMsg::QualityReport: need += (int)sizeof(msg->u.quality_report); break;`n" +
    "         case UdpMsg::QualityReply:  need += (int)sizeof(msg->u.quality_reply); break;`n" +
    "         case UdpMsg::InputAck:      need += (int)sizeof(msg->u.input_ack); break;`n" +
    "         case UdpMsg::KeepAlive:     break;`n" +
    "         case UdpMsg::Input:`n" +
    "            need += (int)((char *)&msg->u.input.bits - (char *)&msg->u.input);`n" +
    "            if (len >= need) {`n" +
    "               need += (msg->u.input.num_bits + 7) / 8;`n" +
    "            }`n" +
    "            break;`n" +
    "         default:`n" +
    "            Log(`"dropping a message of unknown type %d (%d bytes).\n`", msg->hdr.type, len);`n" +
    "            return;`n" +
    "         }`n" +
    "      }`n" +
    "      if (len < need) {`n" +
    "         Log(`"dropping a truncated message (%d bytes, at least %d expected).\n`", len, need);`n" +
    "         return;`n" +
    "      }`n" +
    "   }`n`n" + $old) 'round 3 validation: OnMsg length and type'
$t = Set-Once $t '      if (msg->hdr.magic != _remote_magic_number) {' (
    '      if (msg->hdr.magic != _remote_magic_number || _remote_magic_number == 0) {  // MK9Rollback (round 3): 0 is "handshake not done"') 'round 3 validation: no magic 0'
$t = Set-Once $t '      ASSERT(FALSE && "Unknown UdpMsg type.");' (
    '      Log("%s unknown message type %d.\n", prefix, msg->hdr.type);  // MK9Rollback (round 3): stock asserted') 'round 3 validation: LogMsg'
$t = Set-Once $t '   ASSERT(FALSE && "Invalid msg in UdpProtocol");' (
    '   Log("dropping an invalid message (type %d, %d bytes).\n", msg->hdr.type, len);  // MK9Rollback (round 3): stock asserted') 'round 3 validation: OnInvalid'
# d: the input packet, before anything changes.
$old = "bool`nUdpProtocol::OnInput(UdpMsg *msg, int len)`n{`n"
$t = Set-Once $t $old (
    "// MK9Rollback (round 3): walks an input packet's compressed frames as OnInput's loop does, changing nothing: every read`n" +
    "// inside numBits, every frame ended by its 0 bit, every button index inside size bytes. Returns the number of frames,`n" +
    "// -1 for a stream OnInput must not decode.`n" +
    "static int`nmk9r_count_input_frames(uint8 *bits, int numBits, int size)`n{`n" +
    "   int offset = 0, frames = 0;`n" +
    "   while (offset < numBits) {`n" +
    "      for (;;) {`n" +
    "         if (offset >= numBits) {`n" +
    "            return -1;  // the frame's end bit is missing`n" +
    "         }`n" +
    "         if (!BitVector_ReadBit(bits, &offset)) {`n" +
    "            break;`n" +
    "         }`n" +
    "         if (offset + 1 + BITVECTOR_NIBBLE_SIZE > numBits) {`n" +
    "            return -1;  // half a record`n" +
    "         }`n" +
    "         BitVector_ReadBit(bits, &offset);`n" +
    "         if (BitVector_ReadNibblet(bits, &offset) >= size * 8) {`n" +
    "            return -1;  // a button outside the input`n" +
    "         }`n" +
    "      }`n" +
    "      frames++;`n" +
    "   }`n" +
    "   return frames;`n" +
    "}`n`n" + $old +
    "   if (msg->u.input.num_bits) {  // MK9Rollback (round 3): a packet no GGPO sends is dropped whole, before it changes anything`n" +
    "      const int size = msg->u.input.input_size, start = (int)msg->u.input.start_frame;`n" +
    "      const char *bad = NULL;`n" +
    "      if (msg->u.input.num_bits >= MAX_COMPRESSED_BITS) {`n" +
    "         bad = `"num_bits`";`n" +
    "      } else if (size < 1 || size > (int)sizeof(_last_received_input.bits) || (_mk9r_input_size && size != _mk9r_input_size)) {`n" +
    "         bad = `"input_size`";`n" +
    "      } else if (_last_received_input.frame < 0 ? (start < 0 || start > _mk9r_local_frame + MK9R_FRAME_WINDOW) : start > _last_received_input.frame + 1) {`n" +
    "         bad = `"start_frame`";`n" +
    "      } else if (mk9r_count_input_frames((uint8 *)msg->u.input.bits, msg->u.input.num_bits, size) < 0) {`n" +
    "         bad = `"bits`";`n" +
    "      }`n" +
    "      if (bad) {`n" +
    "         Log(`"dropping a malformed input packet (%s: start_frame %d, num_bits %d, input_size %d; last received frame %d).\n`",`n" +
    "             bad, start, msg->u.input.num_bits, size, _last_received_input.frame);`n" +
    "         return false;`n" +
    "      }`n" +
    "   }`n`n") 'round 3 validation: OnInput checks'
# e: the peer connect status.
$t = Set-Once $t '         ASSERT(remote_status[i].last_frame >= _peer_connect_status[i].last_frame);' (
    "         if (remote_status[i].last_frame < _peer_connect_status[i].last_frame) {  // MK9Rollback (round 3): stock asserted; MAX below keeps ours`n" +
    "            Log(`"peer connect status %d went back (%d < %d).\n`", i, remote_status[i].last_frame, _peer_connect_status[i].last_frame);`n" +
    "         }") 'round 3 validation: connect status'
# f: the input window and the event queue.
$old = '         bool useInputs = currentFrame == _last_received_input.frame + 1;'
$t = Set-Once $t $old ($old + "`n" +
    "         if (useInputs && (currentFrame > _mk9r_local_frame + MK9R_FRAME_WINDOW || _event_queue.size() >= MK9R_EVENT_ROOM)) {`n" +
    "            // MK9Rollback (round 3): further ahead than a GGPO peer runs, or more than the event queue holds: left for a resend`n" +
    "            Log(`"leaving input frames from %d for a resend (local frame %d, %d events queued).\n`", currentFrame, _mk9r_local_frame, _event_queue.size());`n" +
    "            break;`n" +
    "         }") 'round 3 validation: input window'
$t = Set-Once $t "            if (useInputs) {`n               if (on) {" (
    "            if (useInputs && button < _last_received_input.size * 8) {  // MK9Rollback (round 3): never past bits[] (checked above too)`n" +
    "               if (on) {") 'round 3 validation: button index at the write'
# Round 3, timing (2026-11-10 timing audit):
# - ggpo_sync_rtt_ms: the handshake's round trip (the retry timer's own copy is doubled by its backoff). The only round
#   trip known at a session's frame 0, where MK9Rollback's start barrier needs the one-way trip (WaitShift); -1 until a
#   reply was measured, reset with ggpo_rtt_min_ms.
# - Frame advantage: half the lowest round trip was rounded to frames with +1000, whose edges (16.7, 50, 83.3 ms) are the
#   very values a round trip read at a frame's poll takes: 49 ms gave 1 frame and 50 gave 2, on each game separately, so
#   the difference the balance reads carried +-1 (b46 fight 2: a frame given for a lead under 6 ms, then given back by
#   the other game). +500 puts the edges at 25, 58.3 and 91.7 ms, between those values.
$t = Set-Once $t 'extern "C" { int ggpo_rtt_min_ms = -1; }  // MK9Rollback (rc25): their minimum in ms, -1 before the first' (
    'extern "C" { int ggpo_rtt_min_ms = -1; }  // MK9Rollback (rc25): their minimum in ms, -1 before the first' + "`n" +
    'extern "C" { int ggpo_sync_rtt_ms = -1; }  // MK9Rollback (round 3): the handshake''s round trip in ms, -1 before a reply') 'round 3 timing: sync rtt export'
$t = Set-Once $t '      if (sent) mk9r_sync_rtt_ms = (int)(Platform::GetCurrentTimeMS() - sent);' (
    "      if (sent) {`n         mk9r_sync_rtt_ms = (int)(Platform::GetCurrentTimeMS() - sent);`n" +
    "         ggpo_sync_rtt_ms = mk9r_sync_rtt_ms;  // MK9Rollback (round 3): as measured (the timer's copy is doubled on a retry)`n      }") 'round 3 timing: sync rtt measured'
$t = Set-Once $t "   mk9r_rtt_n = 0;  // MK9Rollback (rc25): a new session's endpoints`n   ggpo_rtt_min_ms = -1;" (
    "   mk9r_rtt_n = 0;  // MK9Rollback (rc25): a new session's endpoints`n   ggpo_rtt_min_ms = -1;`n   ggpo_sync_rtt_ms = -1;  // MK9Rollback (round 3)") 'round 3 timing: sync rtt reset (session)'
$t = Set-Once $t "      mk9r_rtt_n = 0;  // MK9Rollback (rc25)`n      ggpo_rtt_min_ms = -1;" (
    "      mk9r_rtt_n = 0;  // MK9Rollback (rc25)`n      ggpo_rtt_min_ms = -1;`n      ggpo_sync_rtt_ms = -1;  // MK9Rollback (round 3)") 'round 3 timing: sync rtt reset (handshake)'
$t = Set-Once $t '   int remoteFrame = _last_received_input.frame + (ggpo_rtt_min_ms * 60 + 1000) / 2000;' (
    '   int remoteFrame = _last_received_input.frame + (ggpo_rtt_min_ms * 60 + 500) / 2000;  // MK9Rollback (round 3): edges at 25, 58.3, 91.7 ms') 'round 3 timing: frame advantage rounding'
# - First input (batch r3b, 2026-11-10): the two handshakes are independent, and a side drops every input packet until
#   its own is done (OnMsg's magic filter). The game that ran first sent its frame-0 input into that; it went out again
#   only after RUNNING_RETRY_INTERVAL (34 ms), so the later game waited 29-32 ms at frame 0 for an input that had been
#   sent before it started, and MK9Rollback's start barrier took that wait for a lead (both games moved a frame, the
#   real 14 ms lead stayed: 17 and 32 rollbacks in 2 of 6 fights). Until the peer's first input is in, pending inputs go
#   out again every ggpo_first_input_retry_ms ('firstretry N', 0 = as before); a few packets of a few bytes per start.
$t = Set-Once $t '#define RUNNING_RETRY_INTERVAL ggpo_running_retry_interval' (
    '#define RUNNING_RETRY_INTERVAL ggpo_running_retry_interval' + "`n" +
    'extern "C" { int ggpo_first_input_retry_ms = 3; }  // MK9Rollback (round 3): ''firstretry N'', 0 = off') 'round 3 timing: first input retry export'
$t = Set-Once $t '      if (!_state.running.last_input_packet_recv_time || _state.running.last_input_packet_recv_time + RUNNING_RETRY_INTERVAL < now) {' (
    "      // MK9Rollback (round 3): until the peer's first input is in, it may still be in its handshake and drop ours`n" +
    "      if (!_state.running.last_input_packet_recv_time || _state.running.last_input_packet_recv_time +`n" +
    "             (ggpo_first_input_retry_ms > 0 && _last_received_input.frame < 0 && _pending_output.size()`n" +
    '                 ? ggpo_first_input_retry_ms : RUNNING_RETRY_INTERVAL) < now) {') 'round 3 timing: first input retry'
[IO.File]::WriteAllText($up, $t)
# h: a datagram longer than the receive buffer.
$t = [IO.File]::ReadAllText($ud)
$t = Set-Once $t '         if (error == WSAECONNRESET && ++mk9r_resets < 64) {  // MK9Rollback (rc25): a stale ICMP error; datagrams may follow' (
    '         if ((error == WSAECONNRESET || error == WSAEMSGSIZE) && ++mk9r_resets < 64) {  // MK9Rollback (rc25): a stale ICMP error, (round 3) or a datagram longer than the buffer; datagrams may follow') 'round 3 validation: udp.cpp oversized datagram'
[IO.File]::WriteAllText($ud, $t)
# Round 3, 3: socket errors.
# - Udp::Init ignored a failed bind (CreateSocket returns INVALID_SOCKET) and the session went on without a socket; its
#   first sendto then failed and asserted. ggpo_bind_failed is the WSA error of that bind (of socket() too: bind then fails
#   with WSAENOTSOCK; -1 should the error read 0) and 0 after each bind that worked: MK9Rollback reads it right after
#   ggpo_start_session and does not run a session without a socket. (The socket keeps stock's SO_REUSEADDR, so a port
#   another socket already holds without SO_EXCLUSIVEADDRUSE still binds: that is not detected.)
# - Udp::SendTo asserted on any sendto error ("Unknown error in sendto": a network going away, WSAENETUNREACH,
#   WSAENOBUFS...), which closed the game. It logs the error (as stock did first) and returns: the datagram is lost as on
#   the network, the inputs go out again with the next packet, and a link that stays down ends in the disconnect timeout.
$t = [IO.File]::ReadAllText($ud)
$t = Set-Once $t 'extern "C" { void (*ggpo_after_send)(void) = 0; int ggpo_bind_loopback = 0; }' (
    'extern "C" { void (*ggpo_after_send)(void) = 0; int ggpo_bind_loopback = 0; }' + "`n" +
    'extern "C" { int ggpo_bind_failed = 0; }  // MK9Rollback (round 3): the WSA error of a failed bind, 0 after one that worked') 'round 3 sockets: flag'
$t = Set-Once $t "         Log(`"Udp bound to port: %d.\n`", port);`n         return s;" (
    "         Log(`"Udp bound to port: %d.\n`", port);`n" +
    "         ggpo_bind_failed = 0;  // MK9Rollback (round 3)`n" +
    "         return s;") 'round 3 sockets: bind worked'
$t = Set-Once $t "   closesocket(s);`n   return INVALID_SOCKET;" (
    "   ggpo_bind_failed = WSAGetLastError();  // MK9Rollback (round 3): of the bind above; read after ggpo_start_session`n" +
    "   if (!ggpo_bind_failed) {`n" +
    "      ggpo_bind_failed = -1;`n" +
    "   }`n" +
    "   Log(`"Udp could not bind port %d (wsaerr: %d).\n`", bind_port, ggpo_bind_failed);`n" +
    "   closesocket(s);`n   return INVALID_SOCKET;") 'round 3 sockets: bind failed'
$t = Set-Once $t '      ASSERT(FALSE && "Unknown error in sendto");' (
    '      return;  // MK9Rollback (round 3): stock asserted (the game closed); the datagram is lost, as on the network') 'round 3 sockets: sendto error'
[IO.File]::WriteAllText($ud, $t)
# Round 3, timed network simulator (test only; 'netsimtimed 1' = ggpo.network.timed, 2026-11-10). The stock simulator
# holds a packet in the sender's queue and looks at that queue only when the sender polls: once a frame in a running
# session, so 'netsim 15' was exactly one frame of one-way trip, released on the sender's frame grid (timing audit:
# "the in-time window has no width"; nothing under a frame could be measured in the two-instance harness). Timed, the
# packet leaves at once with 8 more bytes ('MK9D' + its due time: the system-wide performance counter in 0.1 ms, the
# two test games being on one PC) and the receiver holds it until then, so it is read at the receiver's first poll after
# the due time, as a packet from a real link is. The delay is drawn once per packet (2/3..1 x delay, as stock's range)
# and never lets a packet pass the one before it. Loss, out-of-order and stall windows stay on the sender's side.
$t = [IO.File]::ReadAllText($up)
$mk9rTenths = "static uint32 Mk9rTenths() {  // system-wide clock in 0.1 ms`n" +
    "   static LARGE_INTEGER f = {};`n" +
    "   LARGE_INTEGER t;`n" +
    "   if (!f.QuadPart) {`n" +
    "      QueryPerformanceFrequency(&f);`n" +
    "   }`n" +
    "   QueryPerformanceCounter(&t);`n" +
    "   return (uint32)(t.QuadPart * 10000 / f.QuadPart);`n" +
    "}"
$t = Set-Once $t 'static int mk9r_spike_drop = 0;  // MK9Rollback netsim: the stall window drops packets' (
    'static int mk9r_spike_drop = 0;  // MK9Rollback netsim: the stall window drops packets' + "`n" +
    "// MK9Rollback (round 3) timed netsim: see build_ggpo.ps1`n" +
    "static int mk9r_timed = 0;`nstatic uint32 mk9r_last_due = 0;`n" + $mk9rTenths) 'timed netsim: statics'
$t = Set-Once $t '   mk9r_spike_drop = Platform::GetConfigInt("ggpo.spike.drop");' (
    '   mk9r_spike_drop = Platform::GetConfigInt("ggpo.spike.drop");' + "`n" +
    '   mk9r_timed = Platform::GetConfigInt("ggpo.network.timed");' + "`n" +
    '   mk9r_last_due = 0;') 'timed netsim: config'
$t = Set-Once $t "      if (_send_latency) {`n         // should really come up" (
    "      if (_send_latency && !mk9r_timed) {`n         // should really come up") 'timed netsim: no hold at the sender'
$t = Set-Once $t ("         _udp->SendTo((char *)entry.msg, entry.msg->PacketSize(), 0,`n" +
    "                      (struct sockaddr *)&entry.dest_addr, sizeof entry.dest_addr);") (
    "         if (mk9r_timed && _send_latency) {  // MK9Rollback (round 3) timed netsim: the receiver holds it until due`n" +
    "            char mk9r_buf[sizeof(UdpMsg) + 8];`n" +
    "            int mk9r_n = entry.msg->PacketSize();`n" +
    "            uint32 mk9r_due = Mk9rTenths() + _send_latency * 20 / 3 + rand() % (_send_latency * 10 / 3 + 1);`n" +
    "            if (mk9r_last_due && (int)(mk9r_due - mk9r_last_due) < 0) {`n" +
    "               mk9r_due = mk9r_last_due;  // in order, as the stock queue`n" +
    "            }`n" +
    "            mk9r_last_due = mk9r_due ? mk9r_due : 1;`n" +
    "            memcpy(mk9r_buf, entry.msg, mk9r_n);`n" +
    "            memcpy(mk9r_buf + mk9r_n, `"MK9D`", 4);`n" +
    "            memcpy(mk9r_buf + mk9r_n + 4, &mk9r_due, 4);`n" +
    "            _udp->SendTo(mk9r_buf, mk9r_n + 8, 0, (struct sockaddr *)&entry.dest_addr, sizeof entry.dest_addr);`n" +
    "         } else {`n" +
    "            _udp->SendTo((char *)entry.msg, entry.msg->PacketSize(), 0,`n" +
    "                         (struct sockaddr *)&entry.dest_addr, sizeof entry.dest_addr);`n" +
    "         }") 'timed netsim: send with the due time'
[IO.File]::WriteAllText($up, $t)
$t = [IO.File]::ReadAllText($ud)
$t = Set-Once $t 'extern "C" { int ggpo_bind_failed = 0; }  // MK9Rollback (round 3): the WSA error of a failed bind, 0 after one that worked' (
    'extern "C" { int ggpo_bind_failed = 0; }  // MK9Rollback (round 3): the WSA error of a failed bind, 0 after one that worked' + "`n" +
    "// MK9Rollback (round 3) timed netsim (test only): see build_ggpo.ps1`n" +
    "#include <deque>`n#include <vector>`n" +
    "struct Mk9rHeld { uint32 due; sockaddr_in from; std::vector<uint8> data; };`n" +
    "static std::deque<Mk9rHeld> mk9r_held;`n" +
    "static int mk9r_udp_timed = 0;`n" + $mk9rTenths) 'timed netsim: receiver statics'
$t = Set-Once $t "   _callbacks = callbacks;`n" (
    "   _callbacks = callbacks;`n" +
    "   mk9r_udp_timed = Platform::GetConfigInt(`"ggpo.network.timed`");  // MK9Rollback (round 3) timed netsim`n" +
    "   mk9r_held.clear();`n") 'timed netsim: receiver config'
$t = Set-Once $t "            continue;`n         }`n         break;`n      } else if (len > 0) {" (
    "            continue;`n         }`n" +
    "         while (!mk9r_held.empty() && (int)(mk9r_held.front().due - Mk9rTenths()) <= 0) {  // MK9Rollback (round 3) timed netsim: due now`n" +
    "            Mk9rHeld h = std::move(mk9r_held.front());`n" +
    "            mk9r_held.pop_front();`n" +
    "            _callbacks->OnMsg(h.from, (UdpMsg *)h.data.data(), (int)h.data.size());`n" +
    "         }`n" +
    "         break;`n      } else if (len > 0) {") 'timed netsim: deliver what is due'
$t = Set-Once $t "         UdpMsg *msg = (UdpMsg *)recv_buf;`n         _callbacks->OnMsg(recv_addr, msg, len);" (
    "         if (mk9r_udp_timed && len > 8 && !memcmp(recv_buf + len - 8, `"MK9D`", 4)) {  // MK9Rollback (round 3) timed netsim: held until due`n" +
    "            Mk9rHeld h;`n" +
    "            memcpy(&h.due, recv_buf + len - 4, 4);`n" +
    "            h.from = recv_addr;`n" +
    "            h.data.assign(recv_buf, recv_buf + len - 8);`n" +
    "            mk9r_held.push_back(std::move(h));`n" +
    "            continue;`n" +
    "         }`n" +
    "         UdpMsg *msg = (UdpMsg *)recv_buf;`n         _callbacks->OnMsg(recv_addr, msg, len);") 'timed netsim: hold'
[IO.File]::WriteAllText($ud, $t)
# rc30 (2026-11-12): a read-only review and a sanitizer fuzz run (ggpo_raw_peer's random datagrams against a ggpo_peer
# built with -fsanitize=address,undefined). One block; it reopens the copies patched above. Tests (tests\host):
# ggpo_session_test.cpp (1, 3-7), ggpo_raw_peer.cpp 'seqjump' (2), 'slowack' / 'noack' against ggpo_peer churn=1 (8),
# ggpo_relay.cpp between two ggpo_peer (7).
#  1. OnQualityReply computed the round trip from the pong the peer sent, whatever it was: a pong the peer made up gave a
#     ping of minutes or a negative one, entered the ring behind ggpo_rtt_min_ms, and (ggpo_rtt_min_ms * 60 + 500) / 2000
#     in SetLocalFrameNumber overflowed (UBSan, random datagrams). A reply whose round trip is negative or above
#     MK9R_RTT_MAX (5000 ms; MK9Rollback disconnects after 3000) is dropped: ping, the ring and its minimum keep theirs.
#  2. OnMsg stored a packet's sequence number for sync requests and replies too, which pass neither the magic nor the
#     order check: one 12-byte sync datagram from the peer's address with a sequence number 20000 ahead made every real
#     packet "out of order", and the session ended in the disconnect timeout. Stored only after both checks now, and
#     from the one sync reply that ends the handshake (as stock did: the peer's packets before it were not taken, so
#     the count starts there; a later sync message no longer moves it).
#  3. Poll's constructor creates an event handle and stock had no destructor: one kernel handle stayed open per session.
#  4. UdpProtocol::_state is a union: when the handshake ended, running.last_network_stats_interval still held the
#     handshake's random and was read as a time, so with the clock of rc25 (it starts at 1000 ms per process) a process's
#     first session had no network statistics for up to 33 s (65 s with the 16-bit randoms of 6). The running part is
#     zeroed where the state becomes Running, the only place; its other two members were 0 there already (the
#     handshake's counter has just reached 0, and no input packet is taken before).
#  5. UdpMsg's constructor set the type only, so the bytes no field owns left as the heap had them: a sync request's
#     remote_magic and remote_endpoint (nothing sets them), the bits after num_bits in an input packet's last byte
#     (earlier packets' bits). The whole message is zeroed, 4128 bytes per sent packet: measured in a loop (i686 -O2),
#     new + delete took 34 ns and 50 ns with the memset, so about 2 us per second at 60-120 packets a second.
#  6. Magic numbers and sync randoms came from rand(), which nothing seeds (GGPO's srand is in the DllMain that is renamed
#     below and never runs): every process's first session had magic number 41 and sync random 18467 (ucrt). They now
#     come from a generator of udp_proto.cpp, seeded once per process from the performance counter, the system time and
#     the process and thread ids; the host's rand() sequence is no longer touched by the handshake (the network
#     simulator, test only, still uses rand()).
#  7. The handshake's round trip (ggpo_sync_rtt_ms, MK9Rollback's start barrier) is that of the first reply that matches
#     one of the last 8 requests. Over the Steam bridge the peer's game may start reading later: the requests wait, the
#     first reply measures that wait plus the round trip, and the replies to the fresher requests (the sync kick's among
#     them, sent when the peer's own request shows that it reads now) arrive once the state is Running, where they were
#     ignored. Such a reply still changes nothing but the measurement: with the peer's magic number and a request of the
#     ring, a smaller round trip (>= 0) replaces the stored one.
#  8. SendPendingOutput asserted (the game closes) when the unacknowledged inputs took 4096 bits or more: 63 frames
#     (MK9R_PENDING_MAX) fit only while fewer than 7 input bits change per frame on average (10 bits a change, 1 a
#     frame); reachable with a peer that sends inputs but acknowledges nothing and a local input that changes that much
#     (ggpo_raw_peer noack / slowack against ggpo_peer churn=1: assertion after about 25 frames). The packet now ends at
#     the last frame that fits (the first always does: at most 1441 bits); the receiver acknowledges that frame and the
#     next packet starts there, as after any loss. peer_connect_status still names the newest local frame: a frame is
#     confirmed from the frames received (_local_connect_status), not from it.
$t = [IO.File]::ReadAllText($up)
$t = Set-Once $t 'static const int MK9R_PENDING_MAX = 63;   // unacknowledged inputs _pending_output holds (RingBuffer<GameInput, 64>)' (
    'static const int MK9R_PENDING_MAX = 63;   // unacknowledged inputs _pending_output holds (RingBuffer<GameInput, 64>)' + "`n" +
    'static const int MK9R_RTT_MAX = 5000;     // MK9Rollback (rc30): ms; a quality reply with a round trip above it (or negative) is dropped') 'rc30 quality reply: limit'
$t = Set-Once $t '   _round_trip_time = Platform::GetCurrentTimeMS() - msg->u.quality_reply.pong;' (
    "   {  // MK9Rollback (rc30): pong is whatever the peer sent; a round trip no link has is not taken`n" +
    "      const int mk9r_rtt = (int)(Platform::GetCurrentTimeMS() - msg->u.quality_reply.pong);`n" +
    "      if (mk9r_rtt < 0 || mk9r_rtt > MK9R_RTT_MAX) {`n" +
    "         Log(`"dropping a quality reply with a round trip of %d ms.\n`", mk9r_rtt);`n" +
    "         return false;`n" +
    "      }`n" +
    "      _round_trip_time = mk9r_rtt;`n" +
    "   }") 'rc30 quality reply: range'
$t = Set-Once $t "   _next_recv_seq = seq;`n   LogMsg(`"recv`", msg);" "   LogMsg(`"recv`", msg);" 'rc30 sequence number: not for sync messages'
$t = Set-Once $t ("         Log(`"dropping out of order packet (seq: %d, last seq:%d)\n`", seq, _next_recv_seq);`n" +
    "         return;`n      }`n   }`n") (
    "         Log(`"dropping out of order packet (seq: %d, last seq:%d)\n`", seq, _next_recv_seq);`n" +
    "         return;`n      }`n" +
    "      // MK9Rollback (rc30): stored here, for a packet that passed the magic and the order check (stock: below, for sync`n" +
    "      // messages too, which pass neither)`n" +
    "      _next_recv_seq = seq;`n   }`n") 'rc30 sequence number: after the order check'
$t = Set-Once $t "      _remote_magic_number = msg->hdr.magic;`n" (
    "      _remote_magic_number = msg->hdr.magic;`n" +
    "      // MK9Rollback (rc30): the peer's stream is counted from the reply that ended the handshake, as stock did (its`n" +
    "      // packets before it were not taken, and may be more than MAX_SEQ_DISTANCE if the handshake took minutes)`n" +
    "      _next_recv_seq = msg->hdr.sequence_number;`n") 'rc30 sequence number: from the handshake''s last reply'
$t = Set-Once $t "      _current_state = Running;`n" (
    "      _current_state = Running;`n" +
    "      // MK9Rollback (rc30): _state is a union; its running part still held the handshake's counter and random, and the`n" +
    "      // random was read as the time of the last statistics update`n" +
    "      memset(&_state.running, 0, sizeof _state.running);`n") 'rc30 running state zeroed'
$t = Set-Once $t 'extern "C" { int ggpo_sync_rtt_ms = -1; }  // MK9Rollback (round 3): the handshake''s round trip in ms, -1 before a reply' (
    'extern "C" { int ggpo_sync_rtt_ms = -1; }  // MK9Rollback (round 3): the handshake''s round trip in ms, -1 before a reply' + "`n" +
    "// MK9Rollback (rc30): magic numbers and sync randoms. Stock took them from rand(), which nothing seeds in this build:`n" +
    "// every process started with the same ones. xorshift32, seeded once per process (see build_ggpo.ps1, rc30, 6).`n" +
    "static uint32`nmk9r_random()`n{`n" +
    "   static uint32 s = 0;`n" +
    "   if (!s) {`n" +
    "      LARGE_INTEGER t;`n" +
    "      FILETIME ft;`n" +
    "      QueryPerformanceCounter(&t);`n" +
    "      GetSystemTimeAsFileTime(&ft);`n" +
    "      s = (uint32)t.LowPart ^ ((uint32)t.HighPart * 0x9E3779B1u) ^ (uint32)ft.dwLowDateTime ^`n" +
    "          ((uint32)GetCurrentProcessId() * 0x85EBCA6Bu) ^ ((uint32)GetCurrentThreadId() * 0xC2B2AE35u);`n" +
    "      s ^= s >> 16;`n" +
    "      s *= 0x7FEB352Du;`n" +
    "      s ^= s >> 15;`n" +
    "      s *= 0x846CA68Bu;`n" +
    "      s ^= s >> 16;`n" +
    "      if (!s) {`n" +
    "         s = 0x9E3779B9u;`n" +
    "      }`n" +
    "   }`n" +
    "   s ^= s << 13;`n" +
    "   s ^= s >> 17;`n" +
    "   s ^= s << 5;`n" +
    "   return s;`n" +
    "}") 'rc30 random: generator'
$t = Set-Once $t '      _magic_number = (uint16)rand();' (
    '      _magic_number = (uint16)(mk9r_random() >> 8);  // MK9Rollback (rc30): stock rand(), never seeded') 'rc30 random: magic number'
$t = Set-Once $t '   _state.sync.random = rand() & 0xFFFF;' (
    '   _state.sync.random = (mk9r_random() >> 8) & 0xFFFF;  // MK9Rollback (rc30): stock rand(), never seeded') 'rc30 random: sync random'
$t = Set-Once $t ("      Log(`"Ignoring SyncReply while not synching.\n`");`n" +
    "      return msg->hdr.magic == _remote_magic_number;") (
    "      Log(`"Ignoring SyncReply while not synching.\n`");`n" +
    "      // MK9Rollback (rc30): the reply to a fresher request than the one that ended the handshake still improves the`n" +
    "      // measured round trip (the first reply may have waited for the peer's game to start reading); nothing else changes`n" +
    "      if (msg->hdr.magic == _remote_magic_number && ggpo_sync_rtt_ms >= 0) {`n" +
    "         for (int j = 0; j < 8 && j < mk9r_sync_ring_n; j++) {`n" +
    "            const int r = (mk9r_sync_ring_n - 1 - j) % 8;`n" +
    "            if (mk9r_sync_ring_random[r] == (int)msg->u.sync_reply.random_reply) {`n" +
    "               const int mk9r_sample = (int)(Platform::GetCurrentTimeMS() - mk9r_sync_ring_ms[r]);`n" +
    "               if (mk9r_sample >= 0 && mk9r_sample < ggpo_sync_rtt_ms) {`n" +
    "                  Log(`"late sync reply: the handshake's round trip was %d ms, not %d.\n`", mk9r_sample, ggpo_sync_rtt_ms);`n" +
    "                  ggpo_sync_rtt_ms = mk9r_sample;`n" +
    "                  if (mk9r_sample < mk9r_sync_rtt_ms) {`n" +
    "                     mk9r_sync_rtt_ms = mk9r_sample;`n" +
    "                  }`n" +
    "               }`n" +
    "               break;`n" +
    "            }`n" +
    "         }`n" +
    "      }`n" +
    "      return msg->hdr.magic == _remote_magic_number;") 'rc30 late sync reply'
$t = Set-Once $t "         GameInput &current = _pending_output.item(j);`n" (
    "         GameInput &current = _pending_output.item(j);`n" +
    "         // MK9Rollback (rc30): the frame's size in the packet (10 bits per changed input bit, 1 to end it). Stock wrote`n" +
    "         // every pending frame and asserted below at MAX_COMPRESSED_BITS; the packet now ends at the last frame that`n" +
    "         // fits, and the rest goes out once that frame is acknowledged. The first frame always fits.`n" +
    "         int mk9r_cost = 1;`n" +
    "         if (memcmp(current.bits, last.bits, current.size) != 0) {`n" +
    "            for (i = 0; i < current.size * 8; i++) {`n" +
    "               if (current.value(i) != last.value(i)) {`n" +
    "                  mk9r_cost += 2 + BITVECTOR_NIBBLE_SIZE;`n" +
    "               }`n" +
    "            }`n" +
    "         }`n" +
    "         if (offset + mk9r_cost >= MAX_COMPRESSED_BITS) {`n" +
    "            Log(`"input packet full at %d bits: frames %d-%d go out, %d more wait for an acknowledgement.\n`",`n" +
    "                offset, msg->u.input.start_frame, current.frame - 1, _pending_output.size() - j);`n" +
    "            break;`n" +
    "         }`n") 'rc30 input packet: the frames that fit'
[IO.File]::WriteAllText($up, $t)
$um = Join-Path $work 'lib\ggpo\network\udp_msg.h'
$t = [IO.File]::ReadAllText($um).Replace("`r`n", "`n")
$t = Set-Once $t '#define UDP_MSG_MAX_PLAYERS          4' ('#define UDP_MSG_MAX_PLAYERS          4' + "`n`n" +
    '#include <string.h>  // MK9Rollback (rc30): memset in the constructor') 'rc30 zeroed message: include'
$t = Set-Once $t '   UdpMsg(MsgType t) { hdr.type = (uint8)t; }' (
    "   // MK9Rollback (rc30): zeroed. Stock set the type only, and the bytes no field owns left as the heap had them.`n" +
    '   UdpMsg(MsgType t) { memset(this, 0, sizeof(*this)); hdr.type = (uint8)t; }') 'rc30 zeroed message: constructor'
[IO.File]::WriteAllText($um, $t)
$ph = Join-Path $work 'lib\ggpo\poll.h'
$t = [IO.File]::ReadAllText($ph).Replace("`r`n", "`n")
$t = Set-Once $t "   Poll(void);`n" ("   Poll(void);`n" +
    "   ~Poll(void);  // MK9Rollback (rc30): closes the constructor's event handle`n") 'rc30 poll handle: poll.h'
[IO.File]::WriteAllText($ph, $t)
$pc = Join-Path $work 'lib\ggpo\poll.cpp'
$t = [IO.File]::ReadAllText($pc).Replace("`r`n", "`n")
$t = Set-Once $t "   _handles[_handle_count++] = CreateEvent(NULL, true, false, NULL);`n}`n" (
    "   _handles[_handle_count++] = CreateEvent(NULL, true, false, NULL);`n}`n`n" +
    "// MK9Rollback (rc30): stock had no destructor, and every session left this event handle open. Handles a sink registers`n" +
    "// (none does in this build) stay their owner's.`n" +
    "Poll::~Poll(void)`n{`n" +
    "   if (_handles[0]) {`n" +
    "      CloseHandle(_handles[0]);`n" +
    "      _handles[0] = NULL;`n" +
    "   }`n" +
    "}`n") 'rc30 poll handle: destructor'
[IO.File]::WriteAllText($pc, $t)
# main.cpp's DllMain (GGPO is normally a DLL) would clash with MK9Rollback's: renamed to a plain
# init function (it only seeds rand()).
$mainCpp = Join-Path $work 'lib\ggpo\main.cpp'
$t = [IO.File]::ReadAllText($mainCpp)
if (-not $t.Contains('DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)')) { throw 'main.cpp not as expected' }
[IO.File]::WriteAllText($mainCpp, $t.Replace('DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)', 'GgpoDllMainUnused(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)'))
$cxx = Join-Path $bin 'i686-w64-mingw32-clang++.exe'
$ar = Join-Path $bin 'llvm-ar.exe'
$flags = @('-std=c++17', '-O2', '-fno-exceptions', '-fno-rtti', '-D_WINDOWS', '-w',
           '-I', (Join-Path $work 'include'), '-I', (Join-Path $work 'lib\ggpo'))
$objs = @()
$ErrorActionPreference = 'Continue'
foreach ($f in Get-ChildItem (Join-Path $work 'lib\ggpo') -Recurse -Filter *.cpp | Where-Object { $_.Name -ne 'platform_linux.cpp' }) {
    $o = Join-Path $work ($f.BaseName + '.o')
    & $cxx @flags -c $f.FullName -o $o
    if ($LASTEXITCODE) { throw "compile failed: $($f.Name)" }
    $objs += $o
}
$lib = Join-Path $out 'libggpo.a'
if (Test-Path $lib) { Remove-Item $lib }
& $ar rcs $lib @objs
if ($LASTEXITCODE) { throw 'ar failed' }
Get-Item $lib | Select-Object Name, Length
