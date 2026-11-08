# Scripted combo input for the harness (2026-11-06; the user asked whether real combos lag or crash): dot-source it and
# call ComboLines. Until then every harness fight was random button mashing (4-23 frame holds, tools\two_instance.ps1 /
# tools\noai_rollback.ps1 Chunk), which lands hits but rarely strings, launchers, juggles or specials.
# Masks ('hold P MASK FRAMES'): 1 up, 2 down, 0x10000 forward / 0x20000 back (resolved towards / away from the opponent
# in the game, CommandChannel), attacks 0x10 0x20 0x40 0x80 (MK9's 1-4), 0x8000 block, 0x4000 flip stance
# (block + flip stance = X-ray with a full meter, docs/todo.md 2026-11-02).
# A routine: walk or dash in, then one of dial strings, string into special, launcher (down + 2) and juggle, special
# (d-f, b-f, d-b, f-f, b-b or d-d + a button; a third enhanced with block), special into a juggle string, throw (1+3),
# low attack into a string; then a short pause. The defender stands, blocks or crouch-blocks.
$script:ComboFwd = 0x10000
$script:ComboBack = 0x20000

function ComboBtn([Random]$r) { (0x10, 0x20, 0x40, 0x80)[$r.Next(4)] }

function ComboSpecial([Random]$r, $s) {
    $motions = @(@(2, 0x10000), @(0x20000, 0x10000), @(2, 0x20000), @(0x10000, 0x10000), @(0x20000, 0x20000), @(2, 2))
    $mo = $motions[$r.Next($motions.Count)]
    [void]$s.Add(@($mo[0], 3))
    if ($mo[0] -eq $mo[1]) { [void]$s.Add(@(0, 2)) }  # a double tap needs neutral between
    [void]$s.Add(@($mo[1], 3))
    $b = ComboBtn $r
    if ($r.Next(3) -eq 0) { $b = $b -bor 0x8000 }  # enhanced (one bar)
    [void]$s.Add(@($b, 4))
}

function ComboString([Random]$r, $s, [int]$hits) {
    for ($i = 0; $i -lt $hits; $i++) { [void]$s.Add(@((ComboBtn $r), 3)); [void]$s.Add(@(0, 4 + $r.Next(4))) }
}

# One attacking routine as a list of @(mask, frames). -Xray: approach, then block + flip stance.
function ComboRoutine([Random]$r, [switch]$Xray) {
    $s = New-Object System.Collections.ArrayList
    if ($r.Next(10) -lt 7) { [void]$s.Add(@(0x10000, 8 + $r.Next(24))) }
    else { [void]$s.Add(@(0x10000, 3)); [void]$s.Add(@(0, 3)); [void]$s.Add(@(0x10000, 12)) }  # dash
    if ($Xray) { [void]$s.Add(@(0xC000, 8)); [void]$s.Add(@(0, 30)); return , $s }
    switch ($r.Next(7)) {
        0 { ComboString $r $s (2 + $r.Next(3)) }
        1 { ComboString $r $s 2; ComboSpecial $r $s }
        2 {  # launcher, jump in, air attack, land, string
            [void]$s.Add(@(0x22, 4)); [void]$s.Add(@(0, 12 + $r.Next(8)))
            [void]$s.Add(@(0x10001, 4)); [void]$s.Add(@(0, 8)); [void]$s.Add(@((ComboBtn $r), 3)); [void]$s.Add(@(0, 10))
            ComboString $r $s 3
        }
        3 { ComboSpecial $r $s }
        4 { ComboSpecial $r $s; [void]$s.Add(@(0, 10)); ComboString $r $s 3 }
        5 { [void]$s.Add(@(0x50, 4)) }  # throw
        6 { [void]$s.Add(@(2, 3)); [void]$s.Add(@(((ComboBtn $r) -bor 2), 4)); [void]$s.Add(@(0, 6)); ComboString $r $s 2 }
    }
    [void]$s.Add(@(0, 4 + $r.Next(10)))
    return , $s
}

# 'hold' lines for one player covering exactly $Frames frames. -Attack: combo routines (the last one cut at $Frames);
# else defending: standing, blocking or crouch-blocking. -Xray: the first routine is an X-ray attempt.
function ComboLines([Random]$r, [int]$Player, [int]$Frames, [switch]$Attack, [switch]$Xray) {
    $steps = New-Object System.Collections.ArrayList
    if ($Attack) {
        $first = $true
        while (($steps | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum -lt $Frames) {
            $rt = if ($first -and $Xray) { ComboRoutine $r -Xray } else { ComboRoutine $r }
            $first = $false
            foreach ($st in $rt) { [void]$steps.Add($st) }
        }
    } else {
        $left = $Frames
        while ($left -gt 0) {
            $k = $r.Next(20)
            $m = if ($k -lt 12) { 0 } elseif ($k -lt 17) { 0x8000 } else { 0x8002 }
            $f = [Math]::Min($left, 8 + $r.Next(30))
            [void]$steps.Add(@($m, $f)); $left -= $f
        }
    }
    $out = @(); $used = 0
    foreach ($st in $steps) {
        if ($used -ge $Frames) { break }
        $f = [Math]::Min($st[1], $Frames - $used)
        $out += ('hold {0} {1:X} {2}' -f $Player, $st[0], $f)
        $used += $f
    }
    return $out
}
