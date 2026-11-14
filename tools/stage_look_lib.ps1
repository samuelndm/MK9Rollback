# Helpers for looking at the stage screen (feature plan 4.2) in one offline game: commands to the DLL, the game's own
# finished frame as a picture ('hudshot', works with the PC locked), and the new log lines.
#   . tools\stage_look_lib.ps1 ; Cmd 'stagedemo 3' ; Shot 'demo03' ; NewLog
# The game is started by the caller (tools\launch_game.ps1, then build\clang_stage\MK9RollbackInjector.exe inject).
param([string]$Dir = "$PSScriptRoot\..\build\clang_stage", [string]$Out = "$PSScriptRoot\..\build\stage_run")
$script:dir = (Resolve-Path $Dir).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$script:out = (Resolve-Path $Out).Path
$script:log = Join-Path $script:dir 'MK9Rollback.log'
$script:seen = Join-Path $script:out 'log_lines_seen.txt'
Add-Type -AssemblyName System.Drawing
. "$PSScriptRoot\desk.ps1"

function Cmd([string[]]$lines, [int]$waitMs = 250) {
    for ($w = 0; $w -lt 30 -and (Test-Path "$script:dir\MK9Rollback_cmd.txt"); $w++) { Start-Sleep -Milliseconds 100 }  # the batch before is not taken yet
    Set-Content -Encoding ascii "$script:dir\cmd.tmp" $lines
    Move-Item "$script:dir\cmd.tmp" "$script:dir\MK9Rollback_cmd.txt" -Force
    Start-Sleep -Milliseconds $waitMs
}

# The next finished frame of the game as <name>.png in the output folder; returns the path ('' when no frame came).
function Shot([string]$name) {
    $bmp = Join-Path $script:out "$name.bmp"
    Remove-Item $bmp -ErrorAction SilentlyContinue
    Cmd "hudshot $bmp" 150
    for ($w = 0; $w -lt 40 -and -not (Test-Path $bmp); $w++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $bmp)) { return '' }
    Start-Sleep -Milliseconds 150  # the writer closes the file
    $png = Join-Path $script:out "$name.png"
    for ($t = 0; $t -lt 5; $t++) {
        try { $img = [System.Drawing.Image]::FromFile($bmp); $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose(); break } catch { Start-Sleep -Milliseconds 200 }
    }
    Remove-Item $bmp -ErrorAction SilentlyContinue
    $png
}

# The log lines written since the last call that match the pattern.
function NewLog([string]$pattern = 'STAGE|INPUT|HUDSHOT|CRASH|HANG\?|OVERLAY stage') {
    $from = 0
    if (Test-Path $script:seen) { $from = [int](Get-Content $script:seen) }
    $all = @(Get-Content $script:log -ErrorAction SilentlyContinue)
    Set-Content -Encoding ascii $script:seen $all.Count
    if ($all.Count -le $from) { return }
    $all[$from..($all.Count - 1)] | Where-Object { $_ -match $pattern } | ForEach-Object { if ($_.Length -gt 230) { $_.Substring(0, 230) } else { $_ } }
}

# Which row of a six-row menu list is lit in a frame (the "1 player" menu at 1920x1200: rows 83 px apart from y 340),
# by the mean brightness of each row's band; for telling whether a key press moved the menu's cursor.
function MenuRow([string]$png) {
    $img = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $best = -1; $bestV = 0; $vals = @()
        for ($r = 0; $r -lt 6; $r++) {
            $y0 = 305 + 83 * $r; $sum = 0; $n = 0
            for ($y = $y0; $y -lt $y0 + 70; $y += 5) { for ($x = 150; $x -lt 240; $x += 3) { $c = $img.GetPixel($x, $y); $sum += $c.R + $c.G + $c.B; $n++ } }
            $v = [int]($sum / $n); $vals += $v
            if ($v -gt $bestV) { $bestV = $v; $best = $r }
        }
        '{0} (bands {1})' -f $best, ($vals -join ' ')
    } finally { $img.Dispose() }
}
