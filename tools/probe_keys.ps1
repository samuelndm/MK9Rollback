# Presses candidate keys one at a time in the focused MKKE window and records,
# for each key, the game-frame window during which it was held. Afterwards the
# per-frame trace (MK9Rollback_frames.log) is scanned for P1 mask changes
# inside each window. Output: JSON next to the trace.
param([string]$Trace = "$PSScriptRoot\..\build\clang\MK9Rollback_frames.log",
      [string[]]$KeyNames = @('W','A','S','D','U','I','J','K','L','O','P','Space','Q','E','F','R','H','N','M','OemSemicolon'),
      [int]$HoldMs = 300)
. "$PSScriptRoot\desk.ps1"
$vk = @{ W=0x57; A=0x41; S=0x53; D=0x44; U=0x55; I=0x49; J=0x4A; K=0x4B; L=0x4C; O=0x4F; P=0x50; Space=0x20; Q=0x51;
         E=0x45; F=0x46; R=0x52; H=0x48; N=0x4E; M=0x4D; OemSemicolon=0xBA; Up=0x26; Down=0x28; Left=0x25; Right=0x27;
         Enter=0x0D; Escape=0x1B; Tab=0x09; Shift=0x10; Ctrl=0x11 }

function LastGameFrame {
    $line = Get-Content $Trace -Tail 40 | Where-Object { $_ -like 'FRAME *' } | Select-Object -Last 1
    if ($line -match 'gf=(\d+)') { [int]$Matches[1] } else { -1 }
}

Focus
$windows = @()
foreach ($name in $KeyNames) {
    Start-Sleep -Milliseconds 1200          # > 1 s so the trace (flushed every 60 ticks) is current
    if (-not (IsFg)) { Write-Warning "MKKE lost foreground before key $name; stopping"; break }
    $before = LastGameFrame
    [Desk]::Down($vk[$name]); Start-Sleep -Milliseconds $HoldMs; [Desk]::Up($vk[$name])
    Start-Sleep -Milliseconds 1200
    $after = LastGameFrame
    $windows += [pscustomobject]@{ key = $name; from = $before; to = $after }
}
Start-Sleep -Milliseconds 1500

# Correlate: every FRAME line, P1 cur mask by game frame.
$frames = @{}
foreach ($l in Get-Content $Trace) {
    if ($l -match '^FRAME \d+ gf=(\d+) .*\| P1 v=1 pad=\w+ i=-?\d+ f=\w+ cur=(\w+),(\w+),(\w+)') {
        $frames[[int]$Matches[1]] = @($Matches[2], $Matches[3], $Matches[4])
    }
}
$result = foreach ($w in $windows) {
    $masks = @{}
    for ($g = $w.from; $g -le $w.to; $g++) {
        if ($frames.ContainsKey($g) -and $frames[$g][0] -ne '00000000') { $masks[$frames[$g][0]] = 1 + [int]$masks[$frames[$g][0]] }
    }
    [pscustomobject]@{ key = $w.key; gfFrom = $w.from; gfTo = $w.to;
                       masks = ($masks.GetEnumerator() | ForEach-Object { "$($_.Key)x$($_.Value)" }) -join ' ' }
}
$out = Join-Path (Split-Path $Trace) ("keyprobe-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".json")
$result | ConvertTo-Json | Set-Content -Encoding utf8 $out
$result | Format-Table -AutoSize | Out-String -Width 200
"saved $out"
