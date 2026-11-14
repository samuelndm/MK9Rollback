# Reads one of MK9's cooked packages (.xxx) from the player's own install and lists what is in it; for the stage
# screen's pictures (feature plan 4.2). Nothing of the game is written anywhere unless -Out names a folder.
#   .\tools\xxx_look.ps1 -File <game>\DiscContentPC\Asset\ui_ps_arenaSelect.xxx [-Out build\arena_look] [-Dump]
# The format as read from the file on 2026-11-12 (not from documentation): Unreal Engine 3 package, file version 472,
# licensee 46, the tag "MK  " after the header size, everything after the summary in LZO1X chunks of 128 KiB blocks.
param([Parameter(Mandatory)][string]$File, [string]$Out = '', [switch]$Dump, [string]$Only = '', [switch]$Alpha)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class Lzo {
    // LZO1X decompression (the algorithm of lzo1x_decompress); returns the number of bytes written.
    public static int Decompress(byte[] src, int ip, int srcLen, byte[] dst, int op) {
        int ipEnd = ip + srcLen, start = op, t, m;
        bool firstLiteral = false, matchNext = false;
        if (src[ip] > 17) {
            t = src[ip++] - 17;
            if (t < 4) matchNext = true;
            else { do dst[op++] = src[ip++]; while (--t > 0); firstLiteral = true; }
        } else t = 0;
        while (true) {
            bool toMatch = false;
            if (matchNext) {
                // t literal bytes (1..3) follow a match
                matchNext = false;
                do dst[op++] = src[ip++]; while (--t > 0);
                t = src[ip++];
                toMatch = true;
            } else if (firstLiteral) {
                firstLiteral = false;
                t = src[ip++];
                if (t >= 16) toMatch = true;
                else {
                    m = op - (1 + 0x0800) - (t >> 2) - (src[ip++] << 2);
                    dst[op++] = dst[m++]; dst[op++] = dst[m++]; dst[op++] = dst[m];
                    t = src[ip - 2] & 3;
                    if (t == 0) continue;
                    matchNext = true;
                    continue;
                }
            } else {
                t = src[ip++];
                if (t >= 16) toMatch = true;
                else {
                    if (t == 0) { while (src[ip] == 0) { t += 255; ip++; } t += 15 + src[ip++]; }
                    t += 3;
                    Buffer.BlockCopy(src, ip, dst, op, t); ip += t; op += t;
                    firstLiteral = true;
                    continue;
                }
            }
            if (!toMatch) continue;
            // a match
            if (t >= 64) {
                m = op - 1 - ((t >> 2) & 7) - (src[ip++] << 3);
                t = (t >> 5) - 1;
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) { while (src[ip] == 0) { t += 255; ip++; } t += 31 + src[ip++]; }
                m = op - 1 - ((src[ip] >> 2) + (src[ip + 1] << 6));
                ip += 2;
            } else if (t >= 16) {
                m = op - ((t & 8) << 11);
                t &= 7;
                if (t == 0) { while (src[ip] == 0) { t += 255; ip++; } t += 7 + src[ip++]; }
                m -= (src[ip] >> 2) + (src[ip + 1] << 6);
                ip += 2;
                if (m == op) break;  // end of stream
                m -= 0x4000;
            } else {
                m = op - 1 - (t >> 2) - (src[ip++] << 2);
                dst[op++] = dst[m++]; dst[op++] = dst[m];
                t = src[ip - 2] & 3;
                if (t != 0) matchNext = true;
                continue;
            }
            if (m < start) throw new InvalidDataException("LZO: a match before the block's start");
            dst[op++] = dst[m++]; dst[op++] = dst[m++];
            do dst[op++] = dst[m++]; while (--t > 0);
            t = src[ip - 2] & 3;
            if (t != 0) matchNext = true;
        }
        if (ip != ipEnd) throw new InvalidDataException("LZO: " + (ipEnd - ip) + " input bytes left over");
        return op - start;
    }
}

public class Xxx {
    public byte[] Raw, U;          // the file, and the package as it is uncompressed
    public int Version, Licensee, HeaderSize, NameCount, NameOffset, ExportCount, ExportOffset, ImportCount, ImportOffset, DependsOffset;
    public uint PackageFlags, CompressionFlags;
    public string Tag, Folder;
    public List<int[]> Chunks = new List<int[]>();  // uncompressed offset, uncompressed size, compressed offset, compressed size
    public string[] Names;
    public StringBuilder Log = new StringBuilder();

    static int I32(byte[] b, int o) { return BitConverter.ToInt32(b, o); }

    public Xxx(string path) {
        Raw = File.ReadAllBytes(path);
        if ((uint)I32(Raw, 0) != 0x9E2A83C1u) throw new InvalidDataException("not an Unreal package");
        Version = BitConverter.ToUInt16(Raw, 4); Licensee = BitConverter.ToUInt16(Raw, 6);
        HeaderSize = I32(Raw, 8);
        Tag = Encoding.ASCII.GetString(Raw, 12, 4);
        int o = 20;  // after the tag and one number (62 in the files looked at)
        int len = I32(Raw, o); o += 4;
        Folder = Encoding.ASCII.GetString(Raw, o, len - 1); o += len;
        PackageFlags = (uint)I32(Raw, o); o += 4;
        NameCount = I32(Raw, o); NameOffset = I32(Raw, o + 4);
        ExportCount = I32(Raw, o + 8); ExportOffset = I32(Raw, o + 12);
        ImportCount = I32(Raw, o + 16); ImportOffset = I32(Raw, o + 20);
        DependsOffset = I32(Raw, o + 24); o += 28;
        o += 4;   // one number, 0
        o += 16;  // the package's GUID
        o += 8;   // two numbers (2605 and 139 in the files looked at)
        CompressionFlags = (uint)I32(Raw, o); o += 4;
        int n = I32(Raw, o); o += 4;
        for (int i = 0; i < n; ++i, o += 16) Chunks.Add(new int[] { I32(Raw, o), I32(Raw, o + 4), I32(Raw, o + 8), I32(Raw, o + 12) });
        Log.AppendFormat("version {0}, licensee {1}, tag '{2}', header {3} bytes, folder '{4}', package flags {5:X8}, compression {6}, {7} chunks\r\n",
                         Version, Licensee, Tag, HeaderSize, Folder, PackageFlags, CompressionFlags, n);
        Log.AppendFormat("names {0} at {1}, imports {2} at {3}, exports {4} at {5}, depends at {6}\r\n", NameCount, NameOffset, ImportCount,
                         ImportOffset, ExportCount, ExportOffset, DependsOffset);
        if (n == 0) { U = Raw; return; }
        int[] last = Chunks[n - 1];
        U = new byte[last[0] + last[1]];
        Buffer.BlockCopy(Raw, 0, U, 0, Math.Min(Chunks[0][0], Raw.Length));
        foreach (int[] c in Chunks) Inflate(c[2], c[3], c[0], c[1]);
        Log.AppendFormat("uncompressed: {0} bytes\r\n", U.Length);
    }

    // One compressed chunk: magic, block size, compressed size, uncompressed size, then (compressed, uncompressed) per block.
    public void Inflate(int at, int size, int to, int want) {
        if ((uint)I32(Raw, at) != 0x9E2A83C1u) throw new InvalidDataException("chunk at " + at + " has no magic");
        int block = I32(Raw, at + 4), comp = I32(Raw, at + 8), unc = I32(Raw, at + 12);
        if (unc != want) throw new InvalidDataException("chunk at " + at + ": " + unc + " bytes, the table says " + want);
        int blocks = (unc + block - 1) / block, table = at + 16, data = table + blocks * 8, done = 0;
        for (int i = 0; i < blocks; ++i) {
            int c = I32(Raw, table + i * 8), u = I32(Raw, table + i * 8 + 4);
            int got = (CompressionFlags & 2) != 0 ? Lzo.Decompress(Raw, data, c, U, to + done) : -1;
            if (got != u) throw new InvalidDataException("block " + i + " of the chunk at " + at + ": " + got + " bytes, wanted " + u);
            data += c; done += u;
        }
        if (done != unc || data != at + size) throw new InvalidDataException("chunk at " + at + " does not add up");
    }

    // ---- tables (layouts read from this file: a name is a counted string and 8 bytes of flags, an import 28 bytes, an
    // export 64 bytes) ----
    public class Import { public string ClassPackage, ClassName, Name; public int Outer; }
    public class Export { public int Class, Super, Outer, Size, Offset; public string Name, ClassName; public int Number; }
    public Import[] Imports;
    public Export[] Exports;
    public string NameAt(int o) {
        int i = I32(U, o), n = I32(U, o + 4);
        if (i < 0 || i >= Names.Length) return "?" + i;
        return n > 0 ? Names[i] + "_" + (n - 1) : Names[i];
    }
    public void ReadTables() {
        Names = new string[NameCount];
        int o = NameOffset;
        for (int i = 0; i < NameCount; ++i) {
            int len = I32(U, o); o += 4;
            if (len >= 0) { Names[i] = Encoding.ASCII.GetString(U, o, Math.Max(0, len - 1)); o += len; }
            else { Names[i] = Encoding.Unicode.GetString(U, o, (-len - 1) * 2); o += -len * 2; }
            o += 8;
        }
        if (o != ImportOffset) Log.AppendFormat("NOTE: the name table ends at {0}, the imports start at {1}\r\n", o, ImportOffset);
        Imports = new Import[ImportCount];
        for (int i = 0; i < ImportCount; ++i) {
            int p = ImportOffset + i * 28;
            Imports[i] = new Import { ClassPackage = NameAt(p), ClassName = NameAt(p + 8), Outer = I32(U, p + 16), Name = NameAt(p + 20) };
        }
        Exports = new Export[ExportCount];
        for (int i = 0; i < ExportCount; ++i) {
            int p = ExportOffset + i * 64;
            Export e = new Export { Class = I32(U, p), Super = I32(U, p + 4), Outer = I32(U, p + 8), Name = NameAt(p + 12), Size = I32(U, p + 32), Offset = I32(U, p + 36) };
            e.ClassName = e.Class < 0 && -e.Class - 1 < ImportCount ? Imports[-e.Class - 1].Name : e.Class > 0 ? "export " + e.Class : "Class";
            Exports[i] = e;
        }
    }

    // ---- an object's tagged properties (name, type, size, array index, value), up to "None" ----
    public class Prop { public string Name, Type, Text; public int At, Size; }
    public List<Prop> Props(int at, out int end) {
        List<Prop> list = new List<Prop>();
        int o = at;
        for (int guard = 0; guard < 200; ++guard) {
            string name = NameAt(o); o += 8;
            if (name == "None") break;
            string type = NameAt(o); o += 8;
            int size = I32(U, o); o += 8;  // size, array index
            Prop p = new Prop { Name = name, Type = type, Size = size };
            if (type == "StructProperty") { p.Text = NameAt(o); o += 8; }
            p.At = o;
            if (type == "BoolProperty") { p.Text = I32(U, o) != 0 ? "true" : "false"; o += 4; }
            else {
                if (type == "IntProperty") p.Text = I32(U, o).ToString();
                else if (type == "FloatProperty") p.Text = BitConverter.ToSingle(U, o).ToString("0.###");
                else if (type == "ByteProperty") p.Text = size == 8 ? NameAt(o) : U[o].ToString();
                else if (type == "NameProperty") p.Text = NameAt(o);
                else if (type == "ObjectProperty") p.Text = "object " + I32(U, o);
                else if (type == "StrProperty") p.Text = Encoding.ASCII.GetString(U, o + 4, Math.Max(0, I32(U, o) - 1));
                o += size;
            }
            list.Add(p);
        }
        end = o;
        return list;
    }

    // ---- a Texture2D's first picture. After the properties: two empty data headers (16 bytes each), the number of
    // sizes, then per size a data header (flags, bytes, bytes on disk, the data's place), the data, width and height.
    // In the files looked at the data is inside the object and not compressed again (flags 0). ----
    public class Tex { public int Width, Height, Format, Mips, DataAt, DataSize, Flags; public string Note; }
    public Tex Texture(Export e) {
        int end;
        List<Prop> props = Props(e.Offset, out end);
        Tex t = new Tex();
        foreach (Prop p in props) if (p.Name == "Format") t.Format = U[p.At];
        int o = end;
        for (int i = 0; i < 2; ++i) {
            int size = I32(U, o + 8);
            if (I32(U, o + 12) != o + 16) { t.Note = "the data header " + i + " is not where it was expected"; return t; }
            o += 16 + size;
        }
        t.Mips = I32(U, o); o += 4;
        if (t.Mips < 1 || t.Mips > 16) { t.Note = "sizes: " + t.Mips; return t; }
        t.Flags = I32(U, o); t.DataSize = I32(U, o + 8); t.DataAt = I32(U, o + 12);
        if (t.DataAt != o + 16) { t.Note = "the picture's data is not inside the object"; return t; }
        o += 16 + t.DataSize;
        t.Width = I32(U, o); t.Height = I32(U, o + 4);
        if (t.Flags != 0 || I32(U, o - t.DataSize - 12) != t.DataSize) t.Note = "data flags " + t.Flags.ToString("X");
        return t;
    }
    static void Colors(byte[] b, int o, uint[] c, bool dxt1) {
        int c0 = b[o] | b[o + 1] << 8, c1 = b[o + 2] | b[o + 3] << 8;
        int r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
        int r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
        c[0] = 0xFF000000u | (uint)(r0 << 16 | g0 << 8 | b0);
        c[1] = 0xFF000000u | (uint)(r1 << 16 | g1 << 8 | b1);
        if (!dxt1 || c0 > c1) {
            c[2] = 0xFF000000u | (uint)((2 * r0 + r1) / 3 << 16 | (2 * g0 + g1) / 3 << 8 | (2 * b0 + b1) / 3);
            c[3] = 0xFF000000u | (uint)((r0 + 2 * r1) / 3 << 16 | (g0 + 2 * g1) / 3 << 8 | (b0 + 2 * b1) / 3);
        } else {
            c[2] = 0xFF000000u | (uint)((r0 + r1) / 2 << 16 | (g0 + g1) / 2 << 8 | (b0 + b1) / 2);
            c[3] = 0xFF000000u;
        }
    }
    // Colours only (format 5 = DXT1, 7 = DXT5); the pictures' own alpha is 0 everywhere and is left out. Also reports
    // the lowest and highest alpha the data holds.
    public static bool KeepAlpha;  // -Alpha: the picture keeps the data's alpha (the screen's frame pieces and font sheets)
    public System.Drawing.Bitmap Picture(Tex t, out int alphaLow, out int alphaHigh) {
        alphaLow = 255; alphaHigh = 0;
        bool dxt1 = t.Format == 5;
        int step = dxt1 ? 8 : 16;
        int[] px = new int[t.Width * t.Height];
        uint[] c = new uint[4];
        int[] al = new int[16];
        int o = t.DataAt;
        for (int by = 0; by < t.Height; by += 4)
            for (int bx = 0; bx < t.Width; bx += 4, o += step) {
                for (int i = 0; i < 16; ++i) al[i] = 255;
                if (!dxt1) {
                    int a0 = U[o], a1 = U[o + 1];
                    ulong bits = 0;
                    for (int i = 0; i < 6; ++i) bits |= (ulong)U[o + 2 + i] << (8 * i);
                    for (int i = 0; i < 16; ++i) {
                        int k = (int)(bits >> (3 * i)) & 7, a;
                        if (k == 0) a = a0; else if (k == 1) a = a1;
                        else if (a0 > a1) a = ((8 - k) * a0 + (k - 1) * a1) / 7;
                        else if (k == 6) a = 0; else if (k == 7) a = 255;
                        else a = ((6 - k) * a0 + (k - 1) * a1) / 5;
                        if (a < alphaLow) alphaLow = a;
                        if (a > alphaHigh) alphaHigh = a;
                        al[i] = a;
                    }
                }
                int co = dxt1 ? o : o + 8;
                Colors(U, co, c, dxt1);
                uint idx = BitConverter.ToUInt32(U, co + 4);
                for (int i = 0; i < 16; ++i) {
                    int x = bx + (i & 3), y = by + (i >> 2);
                    if (x < t.Width && y < t.Height)
                        px[y * t.Width + x] = KeepAlpha ? (int)((c[(idx >> (2 * i)) & 3] & 0xFFFFFFu) | ((uint)al[i] << 24)) : (int)c[(idx >> (2 * i)) & 3];
                }
            }
        System.Drawing.Bitmap bmp = new System.Drawing.Bitmap(t.Width, t.Height, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        System.Drawing.Imaging.BitmapData d = bmp.LockBits(new System.Drawing.Rectangle(0, 0, t.Width, t.Height),
                                                           System.Drawing.Imaging.ImageLockMode.WriteOnly, bmp.PixelFormat);
        System.Runtime.InteropServices.Marshal.Copy(px, 0, d.Scan0, px.Length);
        bmp.UnlockBits(d);
        return bmp;
    }
    // Which compressed chunks hold the uncompressed range [at, at + size).
    public string ChunksOf(int at, int size) {
        List<string> l = new List<string>();
        for (int i = 0; i < Chunks.Count; ++i)
            if (Chunks[i][0] < at + size && Chunks[i][0] + Chunks[i][1] > at) l.Add(i + " (" + Chunks[i][3] + " bytes in the file at " + Chunks[i][2] + ")");
        return string.Join(", ", l.ToArray());
    }

    public string Hex(int at, int n) {
        StringBuilder s = new StringBuilder();
        for (int o = at; o < at + n && o < U.Length; o += 16) {
            s.AppendFormat("{0:X6}  ", o);
            for (int i = 0; i < 16; ++i) s.Append(o + i < U.Length ? U[o + i].ToString("X2") + " " : "   ");
            s.Append(' ');
            for (int i = 0; i < 16 && o + i < U.Length; ++i) s.Append(U[o + i] >= 32 && U[o + i] < 127 ? (char)U[o + i] : '.');
            s.Append("\r\n");
        }
        return s.ToString();
    }
}
'@
[Xxx]::KeepAlpha = [bool]$Alpha
$x = New-Object Xxx (Resolve-Path $File).Path
$x.ReadTables()
$x.Log.ToString()
'imports: ' + (($x.Imports | ForEach-Object { "$($_.ClassPackage).$($_.ClassName) $($_.Name)" }) -join '; ')
$i = 0
foreach ($e in $x.Exports) {
    $i++
    if ($Only -and $e.Name -notlike $Only) { continue }
    $end = 0
    $line = '{0,3} {1,-12} {2,-28} {3,9} bytes at {4,9}' -f $i, $e.ClassName, $e.Name, $e.Size, $e.Offset
    if ($e.ClassName -eq 'Texture2D') {
        try {
            $t = $x.Texture($e)
            $line += '  {0}x{1} format {2}, {3} size(s), data {4} bytes{5}' -f $t.Width, $t.Height, $t.Format, $t.Mips, $t.DataSize, $(if ($t.Note) { "  NOTE: $($t.Note)" })
            if ($Out -and -not $t.Note -and $t.Format -in 5, 7) {
                New-Item -ItemType Directory -Force $Out | Out-Null
                $lo = 0; $hi = 0
                $bmp = $x.Picture($t, [ref]$lo, [ref]$hi)
                $bmp.Save((Join-Path (Resolve-Path $Out).Path (($e.Name -replace '[^A-Za-z0-9_]', '_') + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
                $bmp.Dispose()
                $line += "  alpha $lo..$hi  chunk(s) $($x.ChunksOf($e.Offset, $e.Size))"
            }
        } catch { $line += "  not read: $($_.Exception.Message)" }
    }
    $line
    if ($Dump) { $x.Hex($e.Offset, 320) }
}
$global:XxxLast = $x
