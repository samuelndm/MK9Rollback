# Finds every place in MKKE.exe that holds one of the given 32-bit values (any alignment), as virtual addresses:
# vtable slots, function-pointer tables, immediates. Usage: tools\findptr.ps1 0x008ADF90 0x0089F6E0
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Values)
$exe = 'D:\Projects\MK9\analysis\artifacts\DiscContentPC\MKKE.exe'
Add-Type @'
public static class BScan {
    public static System.Collections.Generic.List<int> Find(byte[] b, uint v) {
        var r = new System.Collections.Generic.List<int>();
        byte a0 = (byte)v, a1 = (byte)(v >> 8), a2 = (byte)(v >> 16), a3 = (byte)(v >> 24);
        for (int i = 0; i < b.Length - 4; i++) if (b[i] == a0 && b[i + 1] == a1 && b[i + 2] == a2 && b[i + 3] == a3) r.Add(i);
        return r;
    }
}
'@
$b = [IO.File]::ReadAllBytes($exe)
$pe = [BitConverter]::ToInt32($b, 0x3C); $n = [BitConverter]::ToUInt16($b, $pe + 6); $opt = [BitConverter]::ToUInt16($b, $pe + 20)
$secs = for ($i = 0; $i -lt $n; $i++) {
    $o = $pe + 24 + $opt + 40 * $i
    [pscustomobject]@{ va = [BitConverter]::ToUInt32($b, $o + 12); size = [BitConverter]::ToUInt32($b, $o + 16); raw = [BitConverter]::ToUInt32($b, $o + 20) }
}
foreach ($v in $Values) {
    $t = [uint32]([Convert]::ToUInt32($v.Replace('0x', ''), 16))
    $hits = [BScan]::Find($b, $t)
    if (-not $hits.Count) { '{0:X8}: not found' -f $t }
    foreach ($f in $hits) {
        $s = $secs | Where-Object { $f -ge $_.raw -and $f -lt $_.raw + $_.size } | Select-Object -First 1
        if ($s) { '{0:X8} at VA {1:X8}' -f $t, (0x400000 + $s.va + ($f - $s.raw)) }
    }
}
