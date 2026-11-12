# Updates an installed netplay package from the GitHub repository's latest release.
# The Play Online window (tools\netplay_gui.ps1) dot-sources this file; it also runs on its own:
#   tools\updater.ps1 -Check            -> prints the installed and the latest version
#   tools\updater.ps1 -Apply            -> installs the latest version if it is newer (the game must be closed)
#   tools\updater.ps1 -Restore          -> puts the files of previous_version back (undoes the last update)
#   tools\updater.ps1 -Apply -InstallRoot <folder>   (another installed copy, e.g. a test folder)
# A release is a tag v<yyyyMMdd-HHmm> with two assets: MK9Rollback-netplay-<stamp>.zip and <zip>.sha256 (written by
# tools\package.ps1, published by tools\publish_release.ps1). The zip is installed only if its SHA-256 matches, every
# entry is a file a release holds ($kReleaseFiles) and it holds the same version. The install is all or nothing: the
# files it replaces go to previous_version.new first, any failure puts them back, and only a complete install turns
# previous_version.new into previous_version; version.txt is written last. "Go back to the previous version" in the
# window (Restore-PreviousVersion) undoes it the same way.
# Settings (build\clang\netplay_gui.txt) and logs are not in the zip and stay.
param([switch]$Check, [switch]$Apply, [switch]$Restore, [string]$InstallRoot = '')

$kUpdateRepo = 'samuelndm/MK9Rollback'
$kUpdateAgent = 'MK9Rollback-updater'
# What a release zip may hold (tools\package.ps1 checks its own zip against this list), as paths with backslashes.
$kReleaseFiles = @('^tools\\[^\\]+\.ps1$', '^build\\clang\\MK9Rollback\.dll$', '^build\\clang\\MK9RollbackInjector\.exe$',
    '^docs\\[^\\]+\.md$', '^version\.txt$', '^README\.txt$', '^Play Online\.bat$')
$kReleaseDirs = @('tools\', 'docs\', 'build\', 'build\clang\')
# The files a release holds today, by name (tools\package.ps1 refuses to pack a file that is not listed here): the
# ones the Play Online window unblocks when it opens (Unblock-PackageFiles).
$kPackageFiles = @('Play Online.bat', 'README.txt', 'version.txt', 'build\clang\MK9Rollback.dll', 'build\clang\MK9RollbackInjector.exe',
    'tools\netplay.ps1', 'tools\netplay_gui.ps1', 'tools\launcher_lib.ps1', 'tools\updater.ps1', 'tools\launch_game.ps1',
    'docs\netplay_lan_test.md')

function Get-InstalledVersion([string]$root) {
    $f = Join-Path $root 'version.txt'
    $v = @{ version = ''; dll = '' }
    if (Test-Path -LiteralPath $f) {
        foreach ($line in Get-Content -LiteralPath $f) {
            if ($line -match '^version=(.+)$') { $v.version = $Matches[1].Trim() }
            elseif ($line -match '^dll=(.+)$') { $v.dll = $Matches[1].Trim() }
        }
    }
    $v
}

# The latest release, or $null with the reason in Get-UpdateError.
function Get-LatestRelease([int]$TimeoutSec = 8) {
    $script:UpdateError = ''
    $script:UpdateRateLimited = $false
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $r = Invoke-RestMethod -Uri "https://api.github.com/repos/$kUpdateRepo/releases/latest" -TimeoutSec $TimeoutSec -UseBasicParsing `
            -Headers @{ 'User-Agent' = $kUpdateAgent; 'Accept' = 'application/vnd.github+json' }
    } catch {
        $status = 0
        try { $status = [int]$_.Exception.Response.StatusCode } catch { }
        if ($status -eq 403 -or $status -eq 429) {
            # 60 unauthenticated API calls an hour per address; GitHub answers 403 (or 429) after that.
            $script:UpdateRateLimited = $true
            $script:UpdateError = 'GitHub is limiting update checks; try again in an hour.'
        } else {
            $script:UpdateError = "could not reach GitHub ($($_.Exception.Message))"
        }
        return $null
    }
    $tag = "$($r.tag_name)"
    if ($tag -notmatch '^v(\d{8}-\d{4})$') { $script:UpdateError = "unexpected release tag '$tag'"; return $null }
    $version = $Matches[1]  # now: the -match below replaces $Matches
    $zip = $r.assets | Where-Object { $_.name -match '^MK9Rollback-netplay-\d{8}-\d{4}\.zip$' } | Select-Object -First 1
    $sha = $r.assets | Where-Object { $_.name -eq "$($zip.name).sha256" } | Select-Object -First 1
    if (-not $zip -or -not $sha) { $script:UpdateError = "release $tag has no zip and checksum"; return $null }
    [pscustomobject]@{ Version = $version; Tag = $tag; ZipUrl = $zip.browser_download_url; ZipName = $zip.name
        ShaUrl = $sha.browser_download_url; Page = $r.html_url; Notes = "$($r.body)" }
}

# Why the last Get-LatestRelease returned $null (read from the same scope it was written in, also in a runspace).
function Get-UpdateError { [pscustomobject]@{ Error = "$script:UpdateError"; RateLimited = [bool]$script:UpdateRateLimited } }

# Version stamps (yyyyMMdd-HHmm) compare as text. A package without version.txt predates the updater: older.
function Test-UpdateNewer($release, [string]$installed) {
    $release -and (-not $installed -or [string]::CompareOrdinal($release.Version, $installed) -gt 0)
}

# A zip entry name -> its relative path with backslashes, or $null when a release must not hold it: a way out of the
# folder (.., a leading \ or /, a drive colon) or a file that is not on $kReleaseFiles. Folders return ''.
function Get-ReleaseEntryPath([string]$name) {
    if (-not $name -or $name.Contains('..') -or $name.StartsWith('\') -or $name.StartsWith('/') -or $name.Contains(':')) { return $null }
    $rel = $name.Replace('/', '\')
    if ($rel.EndsWith('\')) { if ($kReleaseDirs -contains $rel) { return '' } else { return $null } }
    foreach ($p in $kReleaseFiles) { if ($rel -match $p) { return $rel } }
    $null
}

# Removes the internet zone mark from the release's files in $root (files extracted from a downloaded zip carry it; the
# injector and the DLL would then be blocked) and from nothing else: the zip has no top folder, so $root can be the
# player's Downloads or Desktop folder, where every other download must keep its mark. $rels: relative paths; one that
# a release must not hold (Get-ReleaseEntryPath) is skipped.
function Unblock-PackageFiles([string]$root, [string[]]$rels = $kPackageFiles) {
    foreach ($rel in $rels) {
        if (-not $rel -or -not (Get-ReleaseEntryPath $rel)) { continue }
        $f = Join-Path $root $rel
        if (-not [System.IO.File]::Exists($f)) { continue }
        try { Unblock-File -LiteralPath $f -ErrorAction Stop } catch { }
    }
}

# The files under $dir as relative paths, built from the names (not by cutting $dir's length off FullName: %TEMP% is
# often an 8.3 short path such as C:\Users\LONGNA~1\..., and Get-ChildItem returns the long one).
function Get-TreeFiles([string]$dir, [string]$prefix = '') {
    foreach ($i in Get-ChildItem -LiteralPath $dir -Force) {
        $rel = if ($prefix) { "$prefix\$($i.Name)" } else { $i.Name }
        if ($i.PSIsContainer) { Get-TreeFiles $i.FullName $rel } else { $rel }
    }
}

# Checks every entry of $zipPath, then extracts it into $dest. Returns '' or the first entry refused.
function Expand-ReleaseZip([string]$zipPath, [string]$dest) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $files = @()
        foreach ($e in $z.Entries) {
            $rel = Get-ReleaseEntryPath $e.FullName
            if ($null -eq $rel) { return $e.FullName }
            if ($rel) { $files += ,@($e, $rel) }
        }
        foreach ($f in $files) {
            $to = Join-Path $dest $f[1]
            New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($f[0], $to, $true)
        }
    } finally { $z.Dispose() }
    ''
}

# Copies $rels (relative paths) from $from into $root and deletes $remove there, all or nothing: everything touched is
# first copied into $backup (with added_files.txt naming the files that did not exist before); on any failure every
# file is put back from $backup, the added ones are deleted, and the error is thrown again. version.txt goes last.
# $backup stays for the caller (it becomes previous_version). $script:InstallCopyOrder lists the copies in order.
function Invoke-FileSwap([string]$from, [string[]]$rels, [string]$root, [string]$backup, [string[]]$remove = @()) {
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    $added = New-Object System.Collections.Generic.List[string]
    foreach ($rel in @($rels) + @($remove)) {
        $cur = Join-Path $root $rel
        if ([System.IO.File]::Exists($cur)) {
            $b = Join-Path $backup $rel
            New-Item -ItemType Directory -Force -Path (Split-Path $b) | Out-Null
            [System.IO.File]::Copy($cur, $b, $true)
        } elseif ($rels -contains $rel) { $added.Add($rel) }
    }
    [System.IO.File]::WriteAllLines((Join-Path $backup 'added_files.txt'), [string[]]$added.ToArray(), [System.Text.Encoding]::ASCII)
    $ordered = @($rels | Where-Object { $_ -ne 'version.txt' }) + @($rels | Where-Object { $_ -eq 'version.txt' })
    $touched = New-Object System.Collections.Generic.List[string]
    $script:InstallCopyOrder = New-Object System.Collections.Generic.List[string]
    try {
        foreach ($rel in $remove) {
            $cur = Join-Path $root $rel
            if (-not [System.IO.File]::Exists($cur)) { continue }
            $touched.Add($rel)
            [System.IO.File]::Delete($cur)
        }
        foreach ($rel in $ordered) {
            if (-not $rel) { continue }
            $dest = Join-Path $root $rel
            $touched.Add($rel)
            New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
            [System.IO.File]::Copy((Join-Path $from $rel), $dest, $true)
            $script:InstallCopyOrder.Add($rel)
        }
    } catch {
        $why = $_.Exception.Message
        $bad = @()
        foreach ($rel in $touched) {
            $cur = Join-Path $root $rel
            $b = Join-Path $backup $rel
            try {
                if ([System.IO.File]::Exists($b)) {
                    $same = [System.IO.File]::Exists($cur) -and (Get-FileHash -LiteralPath $cur).Hash -eq (Get-FileHash -LiteralPath $b).Hash
                    if (-not $same) { [System.IO.File]::Copy($b, $cur, $true) }
                } elseif ($added.Contains($rel) -and [System.IO.File]::Exists($cur)) {
                    [System.IO.File]::Delete($cur)
                }
            } catch { $bad += $rel }
        }
        if ($bad) { throw "$why; these files could not be put back: $($bad -join ', ') (copies are in $(Split-Path -Leaf $backup))" }
        Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue
        throw "$why (nothing was changed)"
    }
}

# Checks and installs a downloaded release zip into $root. $shaText: the release's .sha256 text. $version: the
# release's stamp ('' accepts any). $say: a scriptblock taking one status line. Returns $true when installed.
function Install-UpdateFromZip([string]$zipPath, [string]$shaText, [string]$root, [scriptblock]$say, [string]$version = '') {
    if (-not $say) { $say = { param($s) } }
    $want = ("$shaText".Trim() -split '\s+')[0].ToUpperInvariant()
    $got = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($want -notmatch '^[0-9A-F]{64}$' -or $want -ne $got) { & $say 'The download is damaged (checksum mismatch). Nothing was changed; try again later.'; return $false }
    $work = Join-Path $env:TEMP "mk9r-install-$(Get-Random)"
    $x = Join-Path $work 'x'
    New-Item -ItemType Directory -Force -Path $x | Out-Null
    try {
        $refused = Expand-ReleaseZip $zipPath $x
        if ($refused) { & $say "The download holds a file an update must not have ($refused). Nothing was changed."; return $false }
        $v = Get-InstalledVersion $x
        $dll = Join-Path $x 'build\clang\MK9Rollback.dll'
        $complete = $v.version -and (-not $version -or $v.version -eq $version) -and (Test-Path -LiteralPath $dll)
        if ($complete -and $v.dll) { $complete = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -eq $v.dll.ToUpperInvariant() }
        if (-not $complete) { & $say 'The download is not a complete package. Nothing was changed.'; return $false }
        $rels = @(Get-TreeFiles $x)
        $next = Join-Path $root 'previous_version.new'
        try {
            Invoke-FileSwap $x $rels $root $next
        } catch {
            & $say "The update failed: $($_.Exception.Message). Your files are as they were."
            return $false
        }
        $prev = Join-Path $root 'previous_version'
        if (Test-Path -LiteralPath $prev) { Remove-Item -LiteralPath $prev -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path -LiteralPath $prev) {
            & $say "Updated to version $($v.version). The previous files are in previous_version.new (the old previous_version folder could not be removed)."
        } else {
            Rename-Item -LiteralPath $next -NewName 'previous_version'
            & $say "Updated to version $($v.version). The previous files are in previous_version."
        }
        Unblock-PackageFiles $root $rels  # the files this zip brought, not every file under $root
        return $true
    } catch {
        & $say "The update failed: $($_.Exception.Message). Nothing was changed."
        return $false
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Downloads, checks and installs a release into $root. $say: a scriptblock taking one status line. Returns $true when
# installed; the caller restarts the launcher.
function Install-Update($release, [string]$root, [scriptblock]$say) {
    if (Get-Process MKKE -ErrorAction SilentlyContinue) { & $say 'Close the game first: its files cannot be replaced while it runs.'; return $false }
    $work = Join-Path $env:TEMP "mk9r-update-$($release.Version)-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        & $say "Downloading version $($release.Version)..."
        $old = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'  # Windows PowerShell's progress bar slows the download a lot
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            $zip = Join-Path $work $release.ZipName
            Invoke-WebRequest -Uri $release.ZipUrl -OutFile $zip -UseBasicParsing -TimeoutSec 120 -Headers @{ 'User-Agent' = $kUpdateAgent }
            $shaText = (Invoke-WebRequest -Uri $release.ShaUrl -UseBasicParsing -TimeoutSec 30 -Headers @{ 'User-Agent' = $kUpdateAgent }).Content
        } finally { $ProgressPreference = $old }
        if ($shaText -is [byte[]]) { $shaText = [Text.Encoding]::ASCII.GetString($shaText) }
        return (Install-UpdateFromZip $zip "$shaText" $root $say $release.Version)
    } catch {
        $status = 0
        try { $status = [int]$_.Exception.Response.StatusCode } catch { }
        if ($status -eq 403 -or $status -eq 429) { & $say 'GitHub is limiting downloads; try again in an hour. Nothing was changed.' }
        else { & $say "The download failed: $($_.Exception.Message). Nothing was changed." }
        return $false
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Puts the files of <root>\previous_version back (all or nothing, version.txt last), deletes the files the update had
# added, then removes previous_version. Returns $true when done.
function Restore-PreviousVersion([string]$root, [scriptblock]$say) {
    if (-not $say) { $say = { param($s) } }
    $prev = Join-Path $root 'previous_version'
    if (-not (Test-Path -LiteralPath $prev)) { & $say 'There is no previous version to go back to.'; return $false }
    if (Get-Process MKKE -ErrorAction SilentlyContinue) { & $say 'Close the game first: its files cannot be replaced while it runs.'; return $false }
    $rels = @(Get-TreeFiles $prev | Where-Object { $_ -ne 'added_files.txt' -and (Get-ReleaseEntryPath $_) })
    if (-not $rels) { & $say 'The previous_version folder is empty. Nothing was changed.'; return $false }
    $addedFile = Join-Path $prev 'added_files.txt'
    $remove = @()
    if (Test-Path -LiteralPath $addedFile) { $remove = @(Get-Content -LiteralPath $addedFile | Where-Object { $_ -and (Get-ReleaseEntryPath $_) }) }
    $v = Get-InstalledVersion $prev
    $undo = Join-Path $root 'previous_version.undo'
    try {
        Invoke-FileSwap $prev $rels $root $undo $remove
    } catch {
        & $say "Going back failed: $($_.Exception.Message). Your files are as they were."
        return $false
    }
    Remove-Item -LiteralPath $undo -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $prev -Recurse -Force -ErrorAction SilentlyContinue
    & $say "Back to version $(if ($v.version) { $v.version } else { '(unknown)' })."
    $true
}

if ($Check -or $Apply -or $Restore) {
    if (-not $InstallRoot) { $InstallRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path }
    if ($Restore) { if (Restore-PreviousVersion $InstallRoot { param($s) Write-Host $s }) { exit 0 } else { exit 2 } }
    $inst = Get-InstalledVersion $InstallRoot
    $rel = Get-LatestRelease
    "installed: $(if ($inst.version) { $inst.version } else { '(no version.txt)' }) in $InstallRoot"
    if (-not $rel) { "latest: unknown, $script:UpdateError"; exit 1 }
    "latest: $($rel.Version) ($($rel.Page))"
    if (-not (Test-UpdateNewer $rel $inst.version)) { 'up to date'; exit 0 }
    if ($Apply) { if (Install-Update $rel $InstallRoot { param($s) Write-Host $s }) { exit 0 } else { exit 2 } }
    'an update is available'
}
