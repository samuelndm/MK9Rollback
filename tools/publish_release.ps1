# Publishes a packaged release (tools\package.ps1) as a GitHub release that the players' updater (tools\updater.ps1)
# finds: tag v<stamp> from the zip's name, assets: the package MK9-Rollback-<stamp>.zip, the zip for the updaters of
# the versions before 2026-11 (MK9Rollback-netplay-<stamp>.zip, when it is next to it) and their .sha256 files.
# Run it only for zips that passed the release gate.
#   tools\publish_release.ps1 dist\MK9-Rollback-20261113-0900.zip -Notes 'What changed, for players'
# Needs GitHub CLI signed in (gh auth login) with push rights to the repository in tools\updater.ps1 ($kUpdateRepo).
param([Parameter(Mandatory)][string]$Zip, [string]$Notes = '', [switch]$Draft)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
$zipPath = (Resolve-Path $Zip).Path
$name = Split-Path $zipPath -Leaf
if ($name -notmatch '^MK9-Rollback-(\d{8}-\d{4})\.zip$') { throw "not a package zip name (MK9-Rollback-<stamp>.zip): $name" }
$stamp = $Matches[1]
$legacyPath = Join-Path (Split-Path $zipPath) "MK9Rollback-netplay-$stamp.zip"
$assets = @($zipPath)
if (Test-Path -LiteralPath $legacyPath) { $assets += $legacyPath } else { "NOTE: no $(Split-Path $legacyPath -Leaf) next to the package: players on a version before 2026-11 will not be offered this update" }
foreach ($a in @($assets)) {
    $sha = "$a.sha256"
    if (-not (Test-Path $sha)) { throw "missing $sha (package with tools\package.ps1)" }
    $want = ((Get-Content $sha -TotalCount 1) -split '\s+')[0]
    if ($want -ne (Get-FileHash $a -Algorithm SHA256).Hash) { throw "$sha does not match the zip" }
    $assets += $sha
}
$repo = ((Select-String -Path "$PSScriptRoot\updater.ps1" -Pattern "^\`$kUpdateRepo = '([^']+)'").Matches[0].Groups[1].Value)
if (-not $repo -or $repo -like 'OWNER/*') { throw "set `$kUpdateRepo in tools\updater.ps1 first" }
# The DLL inside, for the release notes; both zips must hold the same one and this version.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$dlls = @()
foreach ($a in @($assets | Where-Object { $_ -like '*.zip' })) {
    $z = [IO.Compression.ZipFile]::OpenRead($a)
    try {
        $e = $z.Entries | Where-Object { $_.FullName -replace '/', '\' -eq 'build\clang\MK9Rollback.dll' } | Select-Object -First 1
        $v = $z.Entries | Where-Object { $_.FullName -eq 'version.txt' } | Select-Object -First 1
        if (-not $e -or -not $v) { throw "$(Split-Path $a -Leaf) has no build\clang\MK9Rollback.dll or version.txt (package it again)" }
        $s = $e.Open(); $dlls += ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($s)) -replace '-', ''); $s.Close()
        $r = New-Object IO.StreamReader($v.Open()); $vt = $r.ReadToEnd(); $r.Close()
        $hasExe = [bool]($z.Entries | Where-Object { $_.FullName -eq 'MK9 Rollback.exe' })
    } finally { $z.Dispose() }
    if ($vt -notmatch "version=$stamp") { throw "version.txt in $(Split-Path $a -Leaf) does not say $stamp" }
    if ($hasExe -ne ($a -eq $zipPath)) { throw "$(Split-Path $a -Leaf): 'MK9 Rollback.exe' belongs in the package and only there" }
}
if (@($dlls | Select-Object -Unique).Count -ne 1) { throw 'the two zips hold different DLLs (package them again, together)' }
$dll = $dlls[0]
$body = @"
$Notes

Install: download **$name**, extract it anywhere, start Steam and open "MK9 Rollback.exe". Already installed? The window offers this update when it opens ("Update now", with the game closed).
Both players need the same version.
$(if ($assets.Count -gt 2) { "`n(MK9Rollback-netplay-$stamp.zip is not a download for players: it is what versions before November 2026 update through.)`n" })
MK9Rollback.dll SHA-256: $dll
"@
$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = 'C:\Program Files\GitHub CLI\gh.exe' }
# The notes go through a file: Windows PowerShell passes double quotes inside an argument to a program unescaped, so
# gh split the text at them and took the pieces for asset names.
$notesFile = Join-Path $env:TEMP "mk9r-release-notes-$stamp.md"
[IO.File]::WriteAllText($notesFile, $body, (New-Object Text.UTF8Encoding($false)))
$ghArgs = @('release', 'create', "v$stamp") + $assets + @('--repo', $repo, '--title', "MK9 Rollback $stamp", '--notes-file', $notesFile)
if ($Draft) { $ghArgs += '--draft' }
try { & $gh @ghArgs } finally { Remove-Item $notesFile -ErrorAction SilentlyContinue }
if ($LASTEXITCODE) { throw "gh release create failed ($LASTEXITCODE)" }
"published v$stamp to $repo"
