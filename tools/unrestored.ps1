# Which memory does one fight frame change that a load does NOT put back?
# Paused at frame S: snapshot all writable pages (A). Step one frame, then 'loadonly 1' restores
# snapshot S without resimulating; snapshot again (B). A vs B, minus paused noise and the DLL's own
# buffers, is the state the tick changed and the rollback left behind. Owners are labelled by the
# DLL ('describefile': MK object type+offset, or UE3 Class:Name+offset).
param([string]$Dir = "$PSScriptRoot\..\build\clang", [string]$Out = "$PSScriptRoot\..\results\unrestored", [int]$Rows = 120)
$ErrorActionPreference = 'Continue'
$ms = "$PSScriptRoot\memscan\memscan.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Cmd([string[]]$lines) { Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines; Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force; Start-Sleep -Milliseconds 800 }
$mpid = (Get-Process MKKE).Id
Cmd @('lab 1', 'objhooks 1', 'rbmode 1')
Start-Sleep 3
Cmd 'pause 1'
Start-Sleep 2
& $ms scan $mpid "$Out\n1.bin" | Out-Null
& $ms scan $mpid "$Out\n2.bin" | Out-Null
& $ms snapfull $mpid "$Out\A.full" | Out-Null
Cmd 'step 1'
Start-Sleep 2
Cmd 'loadonly 1'
Start-Sleep 2
& $ms snapfull $mpid "$Out\B.full" | Out-Null
Remove-Item "$Dir\describe_out.txt", "$Dir\ownranges.txt" -ErrorAction SilentlyContinue
Cmd 'ownranges'
for ($i = 0; $i -lt 20 -and -not (Test-Path "$Dir\ownranges.txt"); $i++) { Start-Sleep 1 }
Start-Sleep 1
Copy-Item "$Dir\ownranges.txt" "$Out\ownranges.txt" -Force
& $ms rbdiff "$Out\A.full" "$Out\B.full" "$Out\n1.bin" "$Out\n2.bin" "$Out\ownranges.txt" "$Out\addrs.txt" $Rows > "$Out\rbdiff.txt"
Copy-Item "$Out\addrs.txt" "$Dir\describe_in.txt" -Force
Cmd 'describefile'
for ($i = 0; $i -lt 180 -and -not (Test-Path "$Dir\describe_out.txt"); $i++) { Start-Sleep 1 }
Start-Sleep 3
Copy-Item "$Dir\describe_out.txt" "$Out\describe_out.txt" -Force
Get-Content "$Out\rbdiff.txt" | Select-Object -Last 1
"--- owners of changed 64-byte chunks (grouped):"
Get-Content "$Out\describe_out.txt" | ForEach-Object { ($_ -split ' ', 2)[1] -replace '\+0x[0-9A-F]+$', '' -replace '_\d+$', '' } |
    Group-Object | Sort-Object Count -Descending | Select-Object -First 40 | ForEach-Object { '{0,5}  {1}' -f $_.Count, $_.Name }
Get-Content "$Dir\MK9Rollback.log" | Select-String 'LOADONLY' | Select-Object -Last 1 | ForEach-Object { $_.Line }
Remove-Item "$Out\A.full", "$Out\B.full" -ErrorAction SilentlyContinue
Cmd 'pause 0'
