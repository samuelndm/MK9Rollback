# One-command netplay for players (instead of writing commands into MK9Rollback_cmd.txt by hand).
# Starts the game through Steam if it is not running, injects MK9Rollback, arms a netplay session for the next fight,
# then shows the session's status lines from the log until the game exits (Ctrl+C stops watching, not the game).
#
#   LAN / internet with forwarded UDP 7000 and 7100:  tools\netplay.ps1 -Peer 192.168.0.12 -Player 1
#   Over Steam (no port forwarding):                  tools\netplay.ps1 -Peer <SteamID64> -Player 2
#   Delay-based instead of rollback:                  add -Lockstep (and a -Delay of 2-3 on LAN, 3-5 over the internet)
#
# Both players then pick the same local Versus fight (same characters on the same sides, same stage and settings).
param([Parameter(Mandatory = $true)][string]$Peer, [ValidateSet(1, 2)][int]$Player = 1, [int]$Delay = 0,
      [switch]$Lockstep, [int]$Port = 7000)
$ErrorActionPreference = 'Continue'
# Every path is taken literally (-LiteralPath): a folder name with [ or ] would be read as a wildcard and match nothing.
$dir = (Resolve-Path -LiteralPath "$PSScriptRoot\..\build\clang").Path
$log = "$dir\MK9Rollback.log"
function Cmd([string[]]$lines) {
    Set-Content -Encoding ascii -LiteralPath "$dir\cmd.tmp" -Value $lines
    Move-Item -LiteralPath "$dir\cmd.tmp" -Destination "$dir\MK9Rollback_cmd.txt" -Force
}

$steam = $Peer -match '^\d{17}$'
if (-not $steam -and -not ($Peer -as [ipaddress])) { "Peer must be an IPv4 address or a 17-digit SteamID64: $Peer"; return }
if (@(Get-Process MKKE -ErrorAction SilentlyContinue).Count -gt 1) { 'More than one MKKE.exe is running; close the extra one.'; return }
if (-not (Get-Process MKKE -ErrorAction SilentlyContinue)) {
    'Starting Mortal Kombat through Steam...'
    & "$PSScriptRoot\launch_game.ps1" | Where-Object { $_ -is [string] } | Select-Object -Last 1
    Start-Sleep 15
}
$injected = (Test-Path -LiteralPath $log) -and ((Get-Item -LiteralPath $log).LastWriteTime -gt (Get-Process MKKE).StartTime)
if (-not $injected) {
    Get-ChildItem -LiteralPath $dir -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    for ($try = 1; $try -le 6 -and -not (Test-Path -LiteralPath $log); $try++) {
        & "$dir\MK9RollbackInjector.exe" inject | Select-Object -Last 1
        for ($w = 0; $w -lt 10 -and -not (Test-Path -LiteralPath $log); $w++) { Start-Sleep 1 }
    }
    if (-not (Test-Path -LiteralPath $log)) { 'Injection failed (see the injector output above).'; return }
}
$start = @(Get-Content -LiteralPath $log).Count
$lines = @($(if ($Lockstep) { 'netcode safe' } else { 'netcode rollback' }))
$lines += if ($steam) { "ggpo steamnetplay $Peer $Player $Delay" } else { "ggpo netplay $Port $Peer $Port $Player $Delay" }
Cmd $lines
"Armed as player $Player against $Peer ($(if ($steam) { 'Steam' } else { 'UDP' }), delay $Delay$(if ($Lockstep) { ', lockstep' })). Pick the same Versus fight on both PCs."
$pattern = 'NETPLAY|GGPO (running|connected|synchronized|disconnected|p2p stopped)|STEAM bridge|CRASH|HANG|ENVIRONMENT|refus'
while (Get-Process MKKE -ErrorAction SilentlyContinue) {
    $all = @(Get-Content -LiteralPath $log)
    if ($all.Count -gt $start) {
        $all[$start..($all.Count - 1)] | Select-String -Pattern $pattern | Where-Object { $_.Line -notmatch 'NETPLAY STATE' } |
            ForEach-Object { $_.Line }
        $start = $all.Count
    }
    Start-Sleep 2
}
'The game has exited.'
