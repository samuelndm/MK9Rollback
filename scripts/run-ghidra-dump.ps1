[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$AddrFile,
    [string]$Project = 'focused-seeds-1',
    [string]$Heap = '3G'
)
# Direct headless Ghidra (NOT REA) against an existing workspace project, -readOnly -noanalysis.
$ErrorActionPreference = 'Stop'
$mk9 = 'D:\Projects\MK9'
$jdk = Join-Path $mk9 'tools\jdk-21.0.12.1+1'
$gh = Join-Path $mk9 'tools\ghidra_12.1.4_PUBLIC'
$java = Join-Path $jdk 'bin\java.exe'
$rt = Join-Path $mk9 "MK9Rollback\analysis\ghidra-runtime\$Name"
foreach ($d in 'home','temp','appdata','localappdata') { New-Item -ItemType Directory -Force (Join-Path $rt $d) | Out-Null }
$outDir = Join-Path $mk9 'MK9Rollback\analysis\ghidra-dumps'
New-Item -ItemType Directory -Force $outDir | Out-Null
$out = Join-Path $outDir "$Name.txt"
$env:JAVA_HOME = $jdk
$env:APPDATA = Join-Path $rt 'appdata'
$env:LOCALAPPDATA = Join-Path $rt 'localappdata'
$iso = @("-Duser.home=$(Join-Path $rt 'home')", "-Djava.io.tmpdir=$(Join-Path $rt 'temp')")
$vm = @(& $java @iso '-cp' (Join-Path $gh 'support\LaunchSupport.jar') 'LaunchSupport' $gh '-vmargs')
$jvm = @($vm) + @("-Xmx$Heap", '-XX:ParallelGCThreads=2', '-XX:CICompilerCount=2', '-Djava.awt.headless=true') + $iso
$a = @('-cp', (Join-Path $gh 'Ghidra\Framework\Utility\lib\Utility.jar'), 'ghidra.Ghidra', 'ghidra.app.util.headless.AnalyzeHeadless',
       (Join-Path $mk9 'analysis\ghidra-projects'), $Project, '-process', 'MKKE.exe', '-noanalysis', '-readOnly',
       '-scriptPath', (Join-Path $mk9 'MK9Rollback\scripts\ghidra'), '-postScript', 'Mk9Pass1Dump.java', $out, (Resolve-Path $AddrFile).Path,
       '-max-cpu', '2', '-log', (Join-Path $rt 'ghidra.log'), '-scriptlog', (Join-Path $rt 'script.log'))
& $java @jvm @a *> (Join-Path $rt 'console.log')
"exit=$LASTEXITCODE out=$out"
