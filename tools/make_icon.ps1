<#
.SYNOPSIS
  Generate a multi-resolution Windows .ico from a source PNG.

.DESCRIPTION
  Resizes the source image to several icon sizes and packs them into an .ico
  container (PNG-compressed entries, supported by Windows Vista+). No external
  tools required — uses System.Drawing.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
  # or with explicit paths:
  powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 -In assets\icon.png -Out assets\icon.ico
#>
param(
    [string]$In  = "assets/icon.png",
    [string]$Out = "assets/icon.ico"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$inPath = (Resolve-Path $In).Path
$outPath = Join-Path (Get-Location).Path $Out

$sizes = 16, 24, 32, 48, 64, 128, 256
$src = [System.Drawing.Image]::FromFile($inPath)

$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($src, 0, 0, $s, $s)
    $g.Dispose()

    $msImg = New-Object System.IO.MemoryStream
    $bmp.Save($msImg, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $pngs += , ($msImg.ToArray())
    $msImg.Dispose()
}
$src.Dispose()

$count = $pngs.Count
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms

# ICONDIR
$bw.Write([uint16]0)       # reserved
$bw.Write([uint16]1)       # type = icon
$bw.Write([uint16]$count)  # image count

# ICONDIRENTRY[count]
$offset = 6 + 16 * $count
for ($i = 0; $i -lt $count; $i++) {
    $s = $sizes[$i]
    $data = $pngs[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }  # 0 means 256 in the ICO format
    $bw.Write([byte]$dim)       # width
    $bw.Write([byte]$dim)       # height
    $bw.Write([byte]0)          # palette count
    $bw.Write([byte]0)          # reserved
    $bw.Write([uint16]1)        # color planes
    $bw.Write([uint16]32)       # bits per pixel
    $bw.Write([uint32]$data.Length)
    $bw.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($data in $pngs) { $bw.Write($data) }
$bw.Flush()

[System.IO.File]::WriteAllBytes($outPath, $ms.ToArray())
$bw.Dispose()
$ms.Dispose()

Write-Output ("Wrote {0} ({1} sizes: {2})" -f $Out, $count, ($sizes -join ', '))
