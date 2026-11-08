# Prints the disassembly of MKKE.exe between two addresses (results\asm\mkke.asm, llvm-objdump without bytes).
#   tools\asm.ps1 0x881360 0x881400
#   tools\asm.ps1 -Callers 0x881360      (every call/jmp to the address)
param([string]$From, [string]$To = '', [string]$Callers = '')
$file = "$PSScriptRoot\..\results\asm\mkke.asm"
if ($Callers) {
    $a = '0x' + ([Convert]::ToUInt32($Callers, 16)).ToString('x')
    Select-String -Path $file -Pattern "(call|j[a-z]+)l?\s+$a\b" | ForEach-Object { $_.Line.Trim() }
    return
}
$lo = [Convert]::ToUInt32($From, 16)
$hi = if ($To) { [Convert]::ToUInt32($To, 16) } else { $lo + 0x100 }
$r = [System.IO.StreamReader]::new($file)
$on = $false
while ($null -ne ($l = $r.ReadLine())) {
    if ($l.Length -lt 10 -or $l[0] -ne ' ') { continue }
    $c = $l.IndexOf(':')
    if ($c -lt 3) { continue }
    $v = 0
    if (-not [uint32]::TryParse($l.Substring(0, $c).Trim(), 'HexNumber', $null, [ref]$v)) { continue }
    if ($v -ge $lo -and $v -le $hi) { $on = $true; $l.Trim() -replace ':\s+', ': ' }
    elseif ($on) { break }
}
$r.Close()
