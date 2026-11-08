# Builds the GGPO stand-in peer (tools/ggpo_peer) into build\clang\ggpo_peer.exe.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build\clang'
$cxx = Join-Path $root '..\tools\llvm-mingw-20260922-ucrt-x86_64\bin\i686-w64-mingw32-clang++.exe'
$ggpoLib = Join-Path $out 'libggpo.a'
if (-not (Test-Path $ggpoLib)) { & (Join-Path $PSScriptRoot 'build_ggpo.ps1') | Out-Null }
$ErrorActionPreference = 'Continue'  # compiler warnings on stderr are not failures
& $cxx -std=c++17 -O2 -static -w -I (Join-Path $root 'build\ggpo-src\include') -o (Join-Path $out 'ggpo_peer.exe') `
    (Join-Path $PSScriptRoot 'ggpo_peer\ggpo_peer.cpp') $ggpoLib -lws2_32 -lwinmm
if ($LASTEXITCODE) { throw 'ggpo_peer build failed' }
Get-Item (Join-Path $out 'ggpo_peer.exe') | Select-Object Name, Length, LastWriteTime
