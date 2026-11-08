# Publishes a packaged zip (tools\package.ps1) as a GitHub release that the players' updater (tools\updater.ps1) finds:
# tag v<stamp> from the zip's name, assets: the zip and its .sha256. Run it only for a zip that passed the release gate.
#   tools\publish_release.ps1 dist\MK9Rollback-netplay-20261108-1450.zip -Notes 'What changed, for players'
# Needs GitHub CLI signed in (gh auth login) with push rights to the repository in tools\updater.ps1 ($kUpdateRepo).
param([Parameter(Mandatory)][string]$Zip, [string]$Notes = '', [switch]$Draft)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
$zipPath = (Resolve-Path $Zip).Path
$name = Split-Path $zipPath -Leaf
if ($name -notmatch '^MK9Rollback-netplay-(\d{8}-\d{4})\.zip$') { throw "not a package zip name: $name" }
$stamp = $Matches[1]
$sha = "$zipPath.sha256"
if (-not (Test-Path $sha)) { throw "missing $sha (package with tools\package.ps1)" }
$want = ((Get-Content $sha -TotalCount 1) -split '\s+')[0]
if ($want -ne (Get-FileHash $zipPath -Algorithm SHA256).Hash) { throw "$sha does not match the zip" }
$repo = ((Select-String -Path "$PSScriptRoot\updater.ps1" -Pattern "^\`$kUpdateRepo = '([^']+)'").Matches[0].Groups[1].Value)
if (-not $repo -or $repo -like 'OWNER/*') { throw "set `$kUpdateRepo in tools\updater.ps1 first" }
# The DLL inside, for the release notes.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $e = $z.Entries | Where-Object { $_.FullName -replace '/', '\' -eq 'build\clang\MK9Rollback.dll' } | Select-Object -First 1
    $v = $z.Entries | Where-Object { $_.FullName -eq 'version.txt' } | Select-Object -First 1
    if (-not $e -or -not $v) { throw "$name has no build\clang\MK9Rollback.dll or version.txt (package it again)" }
    $s = $e.Open(); $dll = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($s)) -replace '-', ''); $s.Close()
    $r = New-Object IO.StreamReader($v.Open()); $vt = $r.ReadToEnd(); $r.Close()
} finally { $z.Dispose() }
if ($vt -notmatch "version=$stamp") { throw "version.txt in the zip does not say $stamp" }
$body = @"
$Notes

Install: extract the zip anywhere, start Steam, double-click "Play Online.bat". Already installed? The Play Online window offers this update when it opens ("Update now", with the game closed).
Both players need the same version.

MK9Rollback.dll SHA-256: $dll
"@
$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = 'C:\Program Files\GitHub CLI\gh.exe' }
# The notes go through a file: Windows PowerShell passes double quotes inside an argument to a program unescaped, so
# gh split the text at them and took the pieces for asset names.
$notesFile = Join-Path $env:TEMP "mk9r-release-notes-$stamp.md"
[IO.File]::WriteAllText($notesFile, $body, (New-Object Text.UTF8Encoding($false)))
$ghArgs = @('release', 'create', "v$stamp", $zipPath, $sha, '--repo', $repo, '--title', "MK9Rollback $stamp", '--notes-file', $notesFile)
if ($Draft) { $ghArgs += '--draft' }
try { & $gh @ghArgs } finally { Remove-Item $notesFile -ErrorAction SilentlyContinue }
if ($LASTEXITCODE) { throw "gh release create failed ($LASTEXITCODE)" }
"published v$stamp to $repo"
