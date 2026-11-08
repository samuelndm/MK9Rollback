# Snapshot coverage analysis: which memory does one fight frame write that the rollback
# snapshot does NOT cover? (Game must be in a fight with MK9Rollback injected.)
param([string]$Dir = "$PSScriptRoot\..\build\clang", [string]$Out = "$PSScriptRoot\..\results\coverage", [int]$Trials = 4)
$ErrorActionPreference = 'Continue'
$ms = "$PSScriptRoot\memscan\memscan.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Cmd([string[]]$lines) { Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines; Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force; Start-Sleep -Milliseconds 600 }
$mpid = (Get-Process MKKE).Id
Cmd 'pause 1'
Start-Sleep 1
for ($k = 1; $k -le $Trials; $k++) {
    & $ms scan $mpid "$Out\t-$k-n1.bin" | Out-Null
    & $ms scan $mpid "$Out\t-$k-n2.bin" | Out-Null
    Cmd 'step 1'
    Start-Sleep 1
    & $ms scan $mpid "$Out\t-$k-s.bin" | Out-Null
    & $ms scan $mpid "$Out\t-$k-n3.bin" | Out-Null
}
& $ms multidiff "$Out\t" $Trials > "$Out\writeset.txt"
Cmd 'spanlist'
Start-Sleep 1
Copy-Item "$Dir\spans.txt" "$Out\spans.txt" -Force
& $ms uncovered "$Out\writeset.txt" "$Out\spans.txt" > "$Out\uncovered.txt"
# Attribute uncovered pages by pointer distance from PlayerInfo/GameInfo (fighters at depth 1).
& $ms ptrpages $mpid "$Out\pages.txt" 4 400 'pinfo=E89D7C:3230' 'gameinfo=E89B70:20C' | Out-Null
$owner = @{}
foreach ($l in Get-Content "$Out\pages.txt") { $p = $l.Split(' '); $owner[$p[0]] = $l }
$rows = foreach ($l in Get-Content "$Out\uncovered.txt") {
    if ($l -match '^\s+(\w+)-(\w+) (\d+) pages') {
        $a = [Convert]::ToUInt32($Matches[1], 16); $b = [Convert]::ToUInt32($Matches[2], 16)
        for ($pg = $a; $pg -lt $b; $pg += 4096) { $key = '{0:X8}' -f $pg; if ($owner.ContainsKey($key)) { $owner[$key] } }
    }
}
Get-Content "$Out\uncovered.txt" | Select-Object -First 1
"uncovered pages reachable from the fighters (depth<=4): $(@($rows).Count)"
$rows | Sort-Object { [int](($_ -split 'd=')[1].Split(' ')[0]) } | Select-Object -First 40
Cmd 'pause 0'
