# Writes launcher\app.ico (the icon of "MK9 Rollback.exe") from the mark the window draws (launcher_lib New-LogoBitmap).
# Run it again after changing the mark, then build: build.ps1 puts the .ico into the exe.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1 [-Preview <folder>]
# -Preview also saves each size as a PNG there (to look at them).
param([string]$Out = '', [string]$Preview = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\launcher_lib.ps1"
Add-Type -AssemblyName System.Drawing
if (-not $Out) { $Out = Join-Path $PSScriptRoot '..\launcher\app.ico' }
$sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
$images = New-Object System.Collections.Generic.List[byte[]]
foreach ($s in $sizes) {
    $bmp = New-LogoBitmap $s
    try {
        if ($Preview) { New-Item -ItemType Directory -Force -Path $Preview | Out-Null; $bmp.Save((Join-Path $Preview "logo_$s.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
        $ms = New-Object System.IO.MemoryStream
        if ($s -ge 256) {
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)   # Windows takes a PNG for the 256 px image
        } else {
            # A 32-bit DIB as an icon holds it: header (height doubled), the pixels bottom-up, then a 1-bit mask.
            $w = New-Object System.IO.BinaryWriter($ms)
            $w.Write([int]40); $w.Write([int]$s); $w.Write([int]($s * 2)); $w.Write([int16]1); $w.Write([int16]32)
            $w.Write([int]0); $w.Write([int]($s * $s * 4)); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0)
            $maskRow = [int]([Math]::Ceiling($s / 32.0) * 4)
            $mask = New-Object byte[] ($maskRow * $s)
            for ($y = $s - 1; $y -ge 0; $y--) {
                for ($x = 0; $x -lt $s; $x++) {
                    $c = $bmp.GetPixel($x, $y)
                    $w.Write([byte]$c.B); $w.Write([byte]$c.G); $w.Write([byte]$c.R); $w.Write([byte]$c.A)
                    if ($c.A -eq 0) { $row = $s - 1 - $y; $mask[$row * $maskRow + [int][Math]::Floor($x / 8)] = $mask[$row * $maskRow + [int][Math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8)) }
                }
            }
            $w.Write($mask); $w.Flush()
        }
        $images.Add($ms.ToArray()); $ms.Dispose()
    } finally { $bmp.Dispose() }
}
$fs = [System.IO.File]::Create((New-Item -ItemType Directory -Force -Path (Split-Path $Out) | ForEach-Object { $Out }))
$bw = New-Object System.IO.BinaryWriter($fs)
try {
    $bw.Write([int16]0); $bw.Write([int16]1); $bw.Write([int16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $bw.Write([byte]$(if ($s -ge 256) { 0 } else { $s })); $bw.Write([byte]$(if ($s -ge 256) { 0 } else { $s }))
        $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([int16]1); $bw.Write([int16]32)
        $bw.Write([int]$images[$i].Length); $bw.Write([int]$offset)
        $offset += $images[$i].Length
    }
    foreach ($img in $images) { $bw.Write($img) }
} finally { $bw.Dispose(); $fs.Dispose() }
"$((Resolve-Path -LiteralPath $Out).Path): $((Get-Item -LiteralPath $Out).Length) bytes, sizes $($sizes -join ', ')"
