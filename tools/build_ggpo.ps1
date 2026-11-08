# Builds GGPO (third_party, unmodified) as a static library for the clang (llvm-mingw i686) DLL build.
# A patched copy of the library sources goes to build\ggpo-src:
#  - udp_proto.h:56 'UdpProtocol::Event(Type t = Unknown)' -> 'Event(...)' (extra qualification only MSVC accepts);
#  - synctest.cpp RaiseSyncError: DebugBreak() (kills the game without a debugger) -> MK9R_GgpoSyncError(buf),
#    implemented by MK9Rollback (logs and counts);
#  - p2p.cpp: lockstep hooks, and AddLocalInput sends the frames a mid-session delay increase padded (below).
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
    if ($n -ne 1) { throw "p2p.cpp $what not as expected ($n matches)" }
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
[IO.File]::WriteAllText($up, $t)
# A failed GGPO assertion showed a message box (behind the fullscreen game: a frozen screen until someone found and
# dismissed it) before exit(0). MK9R_GgpoAssert logs it and closes the game at once.
$pw = Join-Path $work 'lib\ggpo\platform_windows.h'
$t = [IO.File]::ReadAllText($pw)
$old = '   static void AssertFailed(char *msg) { MessageBoxA(NULL, msg, "GGPO Assertion Failed", MB_OK | MB_ICONEXCLAMATION); }'
if (-not $t.Contains($old)) { throw 'platform_windows.h not as expected' }
$t = $t.Replace($old, '   static void AssertFailed(char *msg) { MK9R_GgpoAssert(msg); }')
$t = $t.Replace('class Platform {', "extern `"C`" void MK9R_GgpoAssert(const char* msg);`n`nclass Platform {")
[IO.File]::WriteAllText($pw, $t)
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
