# Builds MK9Rollback.dll and MK9RollbackInjector.exe (x86).
#   .\build.ps1             -> clang (llvm-mingw, workspace-local)
#   .\build.ps1 -Toolchain msvc  -> MSVC via VS 2022 Build Tools (x86 dev shell)
#   .\build.ps1 -Out build\clang_next  -> another output folder (a test running from build\clang keeps its DLL; libggpo.a
#                                          is taken from build\clang)
#   .\build.ps1 -Out build\clang_x -GgpoLib build\ggpo_x\libggpo.a  -> link another GGPO build (tools\build_ggpo.ps1 -Out)
param([ValidateSet('clang', 'msvc')][string]$Toolchain = 'clang', [string]$Out = '', [string]$GgpoLib = '')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = if ($Out) { Join-Path $root $Out } else { Join-Path $root "build\$Toolchain" }
New-Item -ItemType Directory -Force $out | Out-Null

$dllSrc = @('src\dllmain.cpp', 'src\debug\Logger.cpp', 'src\common\Sha256.cpp', 'src\hook\PatternScanner.cpp',
            'src\hook\HookManager.cpp', 'src\hook\CodePatch.cpp', 'src\hook\ObjectHooks.cpp', 'src\hook\Ue3Refs.cpp', 'src\hook\GameLoopHook.cpp', 'src\game\MK9Game.cpp', 'src\game\Input.cpp', 'src\game\Fighter.cpp', 'src\game\MKHeap.cpp',
            'src\debug\CommandChannel.cpp', 'src\debug\CrashLog.cpp', 'src\debug\AllocWatch.cpp', 'src\debug\AnimInputTrace.cpp', 'src\debug\Overlay.cpp', 'src\debug\Hud.cpp', 'src\rollback\StateHash.cpp', 'src\rollback\Snapshot.cpp', 'src\rollback\RollbackManager.cpp', 'src\rollback\FramePace.cpp', 'src\rollback\RayQueries.cpp','src\rollback\FreeQuarantine.cpp', 'src\rollback\ParallelMem.cpp', 'src\rollback\OnlineKeep.cpp', 'src\rollback\CinemaRestore.cpp', 'src\rollback\ActorDefer.cpp', 'src\rollback\ParticleDone.cpp', 'src\rollback\HandleGuard.cpp', 'src\rollback\ActorGuard.cpp', 'src\rollback\ScriptLinkGuard.cpp','src\rollback\PhysWatch.cpp', 'src\debug\Ue3Names.cpp', 'src\rollback\RngTrace.cpp', 'src\rollback\SoundDedupe.cpp', 'src\rollback\RenderTag.cpp','src\rollback\ScriptClock.cpp', 'src\rollback\ScriptRng.cpp', 'src\rollback\CrowdRng.cpp', 'src\rollback\FpuGuard.cpp', 'src\rollback\Rumble.cpp', 'src\net\SteamBridge.cpp', 'src\net\OnlineMode.cpp', 'src\net\HelloProto.cpp', 'src\net\EnvFingerprint.cpp') |
          ForEach-Object { Join-Path $root $_ }
$injSrc = @('injector\Injector.cpp', 'src\common\Sha256.cpp') | ForEach-Object { Join-Path $root $_ }
$def = Join-Path $root 'src\MK9Rollback.def'

$ErrorActionPreference = 'Continue'  # compiler warnings on stderr must not abort; exit codes are checked
if ($Toolchain -eq 'clang') {
    $cxx = Join-Path $root '..\tools\llvm-mingw-20260922-ucrt-x86_64\bin\i686-w64-mingw32-clang++.exe'
    $common = @('-std=c++20', '-O2', '-fno-omit-frame-pointer', '-g', '-Wall', '-Wno-format', '-fno-exceptions', '-fno-rtti', '-static',
                '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN')
    $ggpoLib = if ($GgpoLib) { Join-Path $root $GgpoLib } else { Join-Path $root 'build\clang\libggpo.a' }  # tools\build_ggpo.ps1 writes it there
    if (-not (Test-Path $ggpoLib)) { & (Join-Path $root 'tools\build_ggpo.ps1') | Out-Null }
    & $cxx @common -I (Join-Path $root 'build\ggpo-src\include') -shared -o (Join-Path $out 'MK9Rollback.dll') @dllSrc $def $ggpoLib -lbcrypt -lws2_32 -lwinmm -lgdi32 -liphlpapi `
        "-Wl,--pdb=$(Join-Path $out 'MK9Rollback.pdb')"
    if ($LASTEXITCODE) { throw 'DLL build failed' }
    & $cxx @common -municode -o (Join-Path $out 'MK9RollbackInjector.exe') @injSrc -lbcrypt
    if ($LASTEXITCODE) { throw 'Injector build failed' }
} else {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvarsall shells out to vswhere
    $flags = '/nologo /std:c++20 /O2 /W3 /EHs-c- /GR- /MT /Zi /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS'
    $dllCmd = "cl $flags /LD /Fe`"$out\MK9Rollback.dll`" /Fo`"$out\\`" /Fd`"$out\\`" $($dllSrc -join ' ') /link /DEF:`"$def`" bcrypt.lib /DEBUG"
    $injCmd = "cl $flags /Fe`"$out\MK9RollbackInjector.exe`" /Fo`"$out\\`" /Fd`"$out\\`" $($injSrc -join ' ') /link bcrypt.lib"
    cmd /c "`"$vcvars`" x86 >nul && $dllCmd && $injCmd"
    if ($LASTEXITCODE) { throw 'MSVC build failed' }
}
Get-ChildItem $out -Include *.dll, *.exe -Recurse | Select-Object Name, Length, LastWriteTime




