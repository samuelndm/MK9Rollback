# Two-PC netplay test (Milestones 13–15: LAN, internet, Steam)

Status (2026-10-18): two separate game instances on one PC (`tools/two_instance.ps1`) played whole fights against each other with 0 checksum mismatches, in lockstep and in rollback mode (35 rollbacks in one fight). With one instance slowed down (one core, low priority, delay 0), five start-of-session bugs showed up that would also hit two real PCs. They are fixed (docs/determinism.md, "Different timing between the two games"). So are a missing snapshot range (script event listeners) and a pre-session difference: Toasty's listeners registered in one game only. Whole fights then stayed in sync with one instance slowed, at delay 0 (≈90 rollbacks per side) and at delay 1.

Update (2026-10-25): consecutive rounds now stay in sync too. Round 2 used to split at its first frame for two reasons, both fixed: each game's leftover input history from between rounds (now cleared to the same neutral state at every session start) and the test harness removing the CPU's AI on different frames (the session start now removes AI drones itself). A 3-fight two-instance run with one instance slowed matched every frame of all 4 sessions (about 90 rollbacks per side in the rollback sessions). In single-PC rewind testing, about 3 rewinds in 10,000 replay differently, with no crash; see docs/determinism.md.

**Copying to the second PC:** `tools\package.ps1` builds `dist\MK9Rollback-netplay-<date>.zip` with the DLL, the injector, the scripts and this page, plus a README with the commands. Extract it anywhere on both PCs.

**Untested part, needs you:** until 2026-10-18 21:10 a real controller never reached a netplay session; only the test harness's scripted input did. The DLL now reads every local controller and the keyboard during a session, and sends them as the local player's input. That still needs a person pressing buttons. Quick check on one PC: arm netplay and move. If the fighter does not respond, send the logs (see "What to send back"). The `INPUT capture:` lines show which pads are read. Before that, the path was tested on loopback against a stand-in peer (`tools/ggpo_peer.exe`). This page is the procedure for the first test with two real PCs. It needs two PCs on the same network, each with MK Komplete Edition (Steam) and this repository's build.

## What runs where

Each PC runs a **local Versus** match: the same characters, stage and round settings on both, with the same side for each character.
- PC A controls player 1 and PC B controls player 2.
- The DLL sends each PC's local input to the other through GGPO.
- Each game simulates both fighters and rolls back when the remote input arrives late.

## Easiest: the Play Online window (over Steam)

The package has `Play Online.bat` at the top (in the repository: `tools\Play Online.bat`, which runs `tools\netplay_gui.ps1`). Each player opens it and presses Play; the window starts the game, injects the DLL, arms the mod and shows the log as plain status lines. "Where you fight" has two choices:

- **MK9 Online: Player Match (recommended)**, the default. No codes: both players go to Online > Player Match in the game (next section). "Input delay" is Auto (from the ping) or a fixed 0 to 3 frames; "Safe mode" is lockstep instead of rollback (both players must choose the same).
- **Local Versus over Steam (older test mode)**. Each player sends the code from box 1 (the Steam friend code, the account ID from `HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser`) and pastes the other player's code in box 2 (a friend code or a 17-digit SteamID64, nothing else). The window arms a Steam session (rollback with 2 frames of input delay, or lockstep at delay 3 with "Safe mode"), picks the sides without asking (the lower code is player 1) and re-arms after every fight. The players still pick the same Versus fight in the game (see Playing).

"Save logs to the Desktop" zips the logs for a bug report (send it together with your opponent's zip). Steam IDs become steam-self / steam-peer1, ..., IP addresses lan-ip-N / ip-N, and the Windows user and PC names `<user>` / `<pc>`; PRIVACY.txt in the zip lists what was replaced. "Having trouble?" lists the usual fixes for stutter and connection problems.

## Inside MK9's own online mode (the main mode of "Play Online.bat")

"MK9 Online: Player Match" in the window arms `online auto ping` (or `online auto <frames>` with a fixed input delay) instead of a Versus session. Then:
- **Meeting:** the two players meet through MK9's own Online menu, in a **1 vs 1** Player Match (invite or private room). King of the Hill rooms and Ranked matches always use MK9's own netcode: in King of the Hill, spectators receive the fighters' inputs through the exchange the mod replaces.
- **Handshake:** when an online fight is active and the game's own Steam P2P connection is with the opponent, each DLL sends a hello on Steam channel 79.
- **Takeover:** once both hellos arrive, rollback starts at the first fight frame of the current fight, or of the next one if the current fight has already started.
  - The DLL takes the local side from the game's local pad list, at every fight.
  - It skips MK9's own per-frame input exchange (`0x00885F80`) on both games while a session runs.
  - It keeps the connection's idle timer at 0, so MK9 doesn't drop the opponent while its exchange is quiet.
  - When a fight ends, the session ends with it and is re-armed at once for the next fight of the same match (rematches).
- **Fallback:** when the opponent's game has no mod, the room is not 1 vs 1, or the opponent is anyone else (for example a ranked stranger), nothing changes and MK9's own netcode runs. A session that fails mid-match hands the rest of that match back to MK9's netcode.

**Settled by static analysis (2026-10-26).**
- MK9 restarts its exchange at every fight (StartSendingGamepadData: rings reset, GameFrame 0, a shared RNG seed).
- Its input records carry no frame number, so it resumes cleanly after a session.
- It reads only its own Steam channel.

**Still unknown until the first real test:**
- whether MK9 restarts that exchange at every round or only every fight (every round would mean one short start handshake per round);
- the order of MK9's exchange and our input override within a tick;
- which online script natives the fights call (logged as `ONLINENATIVE`).

The log lines of interest are those starting with `ONLINE`, plus the usual NETPLAY/GGPO lines (see "What to send back").

## Quick start (one command per PC)

`tools\netplay.ps1` starts the game through Steam if needed, injects the DLL, arms the session and then prints the session status. From a Command Prompt in the extracted folder, with the same PowerShell options as `Play Online.bat` (a plain `tools\netplay.ps1 ...` is refused where Windows does not allow scripts, its default):

PC A (its peer is PC B):
```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\netplay.ps1 -Peer 192.168.x.B -Player 1
```
PC B:
```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\netplay.ps1 -Peer 192.168.x.A -Player 2
```

- Over Steam, pass the other player's SteamID64 as `-Peer`; the steps to find it are below.
- `-Delay N` sets the frame delay; `-Lockstep` selects delay-based mode.
- Then pick the same Versus fight on both PCs (see Playing).

The manual steps follow, for reference.

## Setup (both PCs)

1. Copy `build\clang\MK9Rollback.dll` and `MK9RollbackInjector.exe` (plus the folder they're in) to both PCs.
2. Allow UDP ports 7000 and 7100 in the Windows firewall on both PCs (inbound and outbound). 7000 is GGPO; 7100 (GGPO port + 100) carries the desync checksums.
3. Start the game normally through Steam, and leave it at the main menu.
4. Inject: `build\clang\MK9RollbackInjector.exe inject`.
5. Arm netplay by writing one line into `build\clang\MK9Rollback_cmd.txt`:
   - PC A (player 1, its peer is PC B at 192.168.x.B):
     ```
     objhooks 1
     rbmode 1
     ggpo netplay 7000 192.168.x.B 7000 1 0
     ```
   - PC B (player 2, its peer is PC A at 192.168.x.A):
     ```
     objhooks 1
     rbmode 1
     ggpo netplay 7000 192.168.x.A 7000 2 0
     ```
   - The last number is the frame delay. 0 means pure rollback; 1–2 trades latency for fewer rollbacks.

## Playing

1. On both PCs, go to Versus, then local, and pick the **same** characters (P1 on the left, P2 on the right), the same stage and the same settings.
2. Start the fight on both. At the first fight frame the DLL pauses the game. It then:
   - puts both games on the same clocks (GameFrame and the script clock);
   - seeds every RNG identically;
   - connects to the peer, and unpauses when both are running.
   - Expect a short freeze at the fight start. Pad input is ignored until the session runs. Start the fight on both PCs within a minute of each other: after 60 s of waiting, the session is cancelled.
   - The log shows `NETPLAY: GameFrame … renumbered to 100000`, `NETPLAY: first fight frame ...` and then `NETPLAY: both peers running ..., unpaused`.
   - If the two fights did not start from the same state, the session stops right away with `NETPLAY: frame 0 differs from the peer`. Check the characters, sides, stage and settings.
3. Play. Each player uses their own controller or keyboard on their own PC. Every local device drives the local side (P1 on PC A, P2 on PC B), whichever side the game assigned that device to in the menu. Local Versus may need a second device (for example the keyboard) to pick the other side on each PC; during the fight the network supplies that side.

## What to send back

From both PCs: open `Play Online.bat` (the game need not run) and press "Save logs to the Desktop", then send the zip it makes. It holds `build\clang\MK9Rollback.log`, `MK9Rollback_frames.log` and the logs of the session before, with Steam IDs, IP addresses and the Windows user and PC names replaced; the files in `build\clang` themselves are not cleaned, so do not post those. Of interest:
- `GGPO p2p stopped: N frames, N loads, ...`
- any `CRASH?` / `HANG?` lines;
- `NETPLAY checksums: N frames matched the peer, M did not`, plus any `NETPLAY DESYNC: frame ...` lines. Each peer sends the gameplay checksum (life, meter, positions, RNG) of every frame 24 frames after it, when it can no longer be rolled back. The other peer compares it with its own.
- whether both screens showed the same fight. A desync shows as fighters in different places or different life bars on the two PCs.

## Known limits of this first version

- The local input is applied with one frame of delay, on top of the GGPO frame delay.
- Time sync is a simple sleep when GGPO reports the remote is behind.
- Determinism: GGPO SyncTest now fails 0–2 of ~450 rollbacks per session; earlier builds failed 1.5–5% of replayed frames. Now (2026-10-25) about 0.03% of rewinds replay differently (4 of 13,203). Between two instances on one PC, whole fights and round transitions stayed in sync.
- What one PC cannot test: a different CPU or GPU driver, and other machine settings. The DLL pins the FP environment and compares content, DLL build and switches at the session start; a mismatch stops the session (`NETPLAY ENVIRONMENT MISMATCH`).
- Both games must start the fight from the same state. Versus fights do.
  - The netplay start seeds every RNG and holds the seeds while waiting for the peer.
  - It renumbers GameFrame (each PC's intro takes a different number of frames) and rebases the fighters' timers, as MK9's own online mode does.
  - Pads are neutral at the start.
- **Pausing during a session is blocked** (2026-10-19). Every pause of the game goes through `0x00407AB0`. While a session runs, the DLL refuses the game's own pause requests and logs `PAUSE: the game asked to pause during the netplay session (call from …); refused`.
  - Before this, a local pause stopped that PC's frames, and the other PC gave up after 5 s.
  - Tested with a two-instance session: the pause setter was called on one game 10 s into the fight. It was refused, and the whole fight stayed in sync (1,941 / 1,942 frames matched, 0 mismatched).
  - Not tested with a real Start press: the pause menu's screen may still open while the fight keeps running. Close it, and don't use its options during a session.
- If the other game closes or the network drops mid-fight, the session stops about 3 s later. The log shows `NETPLAY: the other player disconnected`. Netplay is disarmed, the controls return to the local controllers and the fight continues locally. Arm it again for a new session. Tested 2026-10-18 by killing one of the two instances 15 s into a fight. Before this change, the session went on with a motionless opponent.
- If the other player never reaches the fight start, this game un-pauses after 60 s and plays the fight locally. The log shows `NETPLAY: the other player did not join within 60 s`, and netplay is disarmed. Tested the same way, with the other instance killed before the start.
- Rollbacks still have a small residual: about 1 in 3,000 replays doesn't reproduce the live frame exactly. Over 2.5 hours of constant rollbacks there is about 1 crash (SyncTest soaks, 2026-10-19: 18,911 loads, 6 real failures, 1 crash). Real sessions roll back far less often than SyncTest. Between two PCs that shows as `NETPLAY DESYNC` lines from some frame on. Delay 1–2 makes rollbacks, and so this risk, rarer; lockstep mode has no replays at all.
- Internet over plain UDP: the same as LAN, with UDP 7000 and 7100 forwarded on the router of the side that receives connections.

## Over Steam (internet without port forwarding, Milestone 15)

GGPO's packets and the checksums go through Steam's P2P networking, using Steam's NAT traversal and relays. This uses the game's own steam_api.dll on private channels (77, 78); MK's online mode and matchmaking are not involved.

1. Each player writes `steamid` into `MK9Rollback_cmd.txt` and reads `STEAM: ... SteamID64 = <17 digits>` from `MK9Rollback.log`. Exchange these numbers.
2. Instead of `ggpo netplay ...`, arm with the peer's SteamID64:
   - PC A (player 1): `ggpo steamnetplay <SteamID64 of B> 1 0`
   - PC B (player 2): `ggpo steamnetplay <SteamID64 of A> 2 0`
3. Start the same local Versus fight on both PCs, as above.
4. The log shows `STEAM bridge: self ... <-> peer ...`, the GGPO connected/synchronized/running lines, and on stop `STEAM bridge stopped: N packets sent, M received`.

The two players must be different Steam accounts. Loopback test (one PC, own SteamID as the peer): Steam delivered the packets back (1122 sent / 1121 received). GGPO connected, synchronized and ran 556 frames against itself, and the checksum link matched 532 frames.

## Lockstep mode (delay-based, no rollback): added 2026-10-15

On both PCs, before the netplay command:

    ggpo lockstep 1

Then use the same `ggpo netplay ...` / `ggpo steamnetplay ...` commands as above. Use a delay of 2–3 on LAN and 3–5 over the internet; 0 is raised to 2.

- **How it works:** GGPO's prediction window is 1, so a frame only runs once the remote input for it has arrived. Nothing is predicted or rolled back, and no snapshots are taken. The rollback crash and desync risks don't apply.
- **Cost:** input delay, plus a short stall when the other side's inputs are late.
- **Checksums:** the side channel still compares them every frame. `NETPLAY DESYNC` lines in the log mean the game itself is not deterministic between the PCs.
- **Turning it off:** `ggpo lockstep 0` returns to rollback.

## Network display and troubleshooting

In a fight the display is part of the HUD, under the round timer: three connection bars, the round trip (`22 MS`), `R` with the
most frames rolled back in the last second (not in Safe mode) and `D` with the input delay. The bars are green (round trip up to
60 ms, little rollback, no stalls), yellow (61-120 ms, 3-4 frames of rollback, or a wait of 150 ms or more for the opponent) or red
(above 120 ms, 5+ frames of rollback, a wait of 500 ms or more, or an opponent that stopped responding). A Wi-Fi mark stands at the
side of each player whose PC is on Wi-Fi. A note under the group explains a hiccup for a few seconds ("Connection hiccup", "This PC
hitched" when the freeze came from this PC, "Opponent not responding"). Before the fight (character select, loading) a badge says
`ROLLBACK READY` with the ping, or why MK9's own netcode will play. In a fight on MK9's netcode a small `MK9 NETCODE` tag stands in
the group's place, with the reason for the first seconds. The display hides with the game's HUD in a finisher. F9 switches to the
detailed line at the top left (rtt, ping, rollback depths, loads, kbps, queue, waits) and back; the window's "Network display" box
chooses the start state (In the HUD, Detailed or Off, sent to the game as `overlay 1|2|0`). When a session stops, a note says why in
plain words and that MK9's own netcode plays the rest of the match.

If the game stutters, freezes or will not connect:
- Allow MKKE.exe through the Windows firewall (Windows Security > Firewall > Allow an app).
- Add the MK9 Rollback folder to your antivirus exclusions.
- Run the game from an SSD, and turn off disk sleep.
- On a laptop: plug it in and set the Windows power mode to "Best performance".
- Close browsers and other heavy apps (downloads, streaming, launchers).
- Use a wired (Ethernet) connection instead of Wi-Fi.
- Both players need the same version ("Check for updates" in the window).
- In a fight, F9 switches between the HUD display and the detailed network line.
