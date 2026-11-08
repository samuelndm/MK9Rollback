# Updates an installed netplay package from the GitHub repository's latest release.
# The Play Online window (tools\netplay_gui.ps1) dot-sources this file; it also runs on its own:
#   tools\updater.ps1 -Check            -> prints the installed and the latest version
#   tools\updater.ps1 -Apply            -> installs the latest version if it is newer (the game must be closed)
#   tools\updater.ps1 -Apply -InstallRoot <folder>   (another installed copy, e.g. a test folder)
# A release is a tag v<yyyyMMdd-HHmm> with two assets: MK9Rollback-netplay-<stamp>.zip and <zip>.sha256 (written by
# tools\package.ps1, published by tools\publish_release.ps1). The zip is installed only if its SHA-256 matches and it
# holds the same version; the files it replaces are kept in <root>\previous_version (put them back to undo).
# Settings (build\clang\netplay_gui.txt) and logs are not in the zip and stay.
param([switch]$Check, [switch]$Apply, [string]$InstallRoot = '')

$kUpdateRepo = 'samuelndm/MK9Rollback'
$kUpdateAgent = 'MK9Rollback-updater'

function Get-InstalledVersion([string]$root) {
    $f = Join-Path $root 'version.txt'
    $v = @{ version = ''; dll = '' }
    if (Test-Path $f) {
        foreach ($line in Get-Content $f) {
            if ($line -match '^version=(.+)$') { $v.version = $Matches[1].Trim() }
            elseif ($line -match '^dll=(.+)$') { $v.dll = $Matches[1].Trim() }
        }
    }
    $v
}

# The latest release, or $null with $script:UpdateError set.
function Get-LatestRelease([int]$TimeoutSec = 8) {
    $script:UpdateError = ''
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $r = Invoke-RestMethod -Uri "https://api.github.com/repos/$kUpdateRepo/releases/latest" -TimeoutSec $TimeoutSec -UseBasicParsing `
            -Headers @{ 'User-Agent' = $kUpdateAgent; 'Accept' = 'application/vnd.github+json' }
    } catch {
        $script:UpdateError = "could not reach GitHub ($($_.Exception.Message))"
        return $null
    }
    $tag = "$($r.tag_name)"
    if ($tag -notmatch '^v(\d{8}-\d{4})$') { $script:UpdateError = "unexpected release tag '$tag'"; return $null }
    $zip = $r.assets | Where-Object { $_.name -match '^MK9Rollback-netplay-\d{8}-\d{4}\.zip$' } | Select-Object -First 1
    $sha = $r.assets | Where-Object { $_.name -eq "$($zip.name).sha256" } | Select-Object -First 1
    if (-not $zip -or -not $sha) { $script:UpdateError = "release $tag has no zip and checksum"; return $null }
    [pscustomobject]@{ Version = $Matches[1]; Tag = $tag; ZipUrl = $zip.browser_download_url; ZipName = $zip.name
        ShaUrl = $sha.browser_download_url; Page = $r.html_url; Notes = "$($r.body)" }
}

# Version stamps (yyyyMMdd-HHmm) compare as text. A package without version.txt predates the updater: older.
function Test-UpdateNewer($release, [string]$installed) {
    $release -and (-not $installed -or [string]::CompareOrdinal($release.Version, $installed) -gt 0)
}

# Downloads, checks and installs a release into $root. $say: a scriptblock taking one status line. Returns $true when
# installed; the caller restarts the launcher.
function Install-Update($release, [string]$root, [scriptblock]$say) {
    if (Get-Process MKKE -ErrorAction SilentlyContinue) { & $say 'Close the game first: its files cannot be replaced while it runs.'; return $false }
    $work = Join-Path $env:TEMP "mk9r-update-$($release.Version)-$(Get-Random)"
    New-Item -ItemType Directory -Force $work | Out-Null
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
        $want = ("$shaText".Trim() -split '\s+')[0].ToUpperInvariant()
        $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($want -notmatch '^[0-9A-F]{64}$' -or $want -ne $got) { & $say 'The download is damaged (checksum mismatch). Nothing was changed; try again later.'; return $false }
        $x = Join-Path $work 'x'
        Expand-Archive -Path $zip -DestinationPath $x -Force
        $v = Get-InstalledVersion $x
        if ($v.version -ne $release.Version -or -not (Test-Path (Join-Path $x 'build\clang\MK9Rollback.dll'))) {
            & $say 'The download is not a complete package. Nothing was changed.'; return $false
        }
        # Keep what is replaced, then copy the new files over (settings and logs are not in the zip).
        $backup = Join-Path $root 'previous_version'
        Remove-Item $backup -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $backup | Out-Null
        $new = @(Get-ChildItem $x -Recurse -File)
        foreach ($f in $new) {
            $rel = $f.FullName.Substring($x.Length + 1)
            $cur = Join-Path $root $rel
            if (Test-Path $cur) {
                New-Item -ItemType Directory -Force (Split-Path (Join-Path $backup $rel)) | Out-Null
                Copy-Item $cur (Join-Path $backup $rel) -Force
            }
        }
        foreach ($f in $new) {
            $rel = $f.FullName.Substring($x.Length + 1)
            $dest = Join-Path $root $rel
            New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
            Copy-Item $f.FullName $dest -Force
        }
        Get-ChildItem $root -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue
        & $say "Updated to version $($release.Version). The previous files are in previous_version."
        return $true
    } catch {
        & $say "The update failed: $($_.Exception.Message). Your files are unchanged or in previous_version."
        return $false
    } finally {
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($Check -or $Apply) {
    if (-not $InstallRoot) { $InstallRoot = (Resolve-Path "$PSScriptRoot\..").Path }
    $inst = Get-InstalledVersion $InstallRoot
    $rel = Get-LatestRelease
    "installed: $(if ($inst.version) { $inst.version } else { '(no version.txt)' }) in $InstallRoot"
    if (-not $rel) { "latest: unknown, $script:UpdateError"; exit 1 }
    "latest: $($rel.Version) ($($rel.Page))"
    if (-not (Test-UpdateNewer $rel $inst.version)) { 'up to date'; exit 0 }
    if ($Apply) { if (Install-Update $rel $InstallRoot { param($s) Write-Host $s }) { exit 0 } else { exit 2 } }
    'an update is available'
}
