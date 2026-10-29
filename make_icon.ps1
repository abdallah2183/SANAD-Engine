# make_icon.ps1 - builds the SANAD application icon (Editor/resources/SANAD.ico)
# from the brand mark Editor/resources/SANAD.png.
#
# The PNG is the single source of the logo: the launcher sidebar, the editor
# toolbar and the About window draw that same file, so the taskbar icon is
# generated from it rather than redrawn by hand (the previous version of this
# script drew an unrelated cube that no longer matched the brand).
#
# Every size Windows asks for is emitted. The old icon held ONE 256 px entry,
# so the 16 px title-bar and 32 px taskbar icons were whatever Windows' own
# downscaler made of it - a blur. Each size here is resampled with
# high-quality bicubic filtering and stored as a PNG entry (supported by every
# Windows since Vista, and by rc.exe).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot 'Editor\resources'
$srcPath = Join-Path $resDir 'SANAD.png'
$icoPath = Join-Path $resDir 'SANAD.ico'
if (-not (Test-Path $srcPath)) { throw "Missing brand mark: $srcPath" }

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$src = [System.Drawing.Image]::FromFile($srcPath)
$entries = @()
try {
    foreach ($s in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.DrawImage($src, 0, 0, $s, $s)
        $g.Dispose()
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $entries += , @{ Size = $s; Bytes = $ms.ToArray() }
        $ms.Dispose()
    }
} finally {
    $src.Dispose()
}

# ICONDIR (6 bytes) + ICONDIRENTRY (16 bytes each) + image data.
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([UInt16]0)                 # reserved
$w.Write([UInt16]1)                 # type: icon
$w.Write([UInt16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }   # 0 means 256
    $w.Write([Byte]$dim)            # width
    $w.Write([Byte]$dim)            # height
    $w.Write([Byte]0)               # palette
    $w.Write([Byte]0)               # reserved
    $w.Write([UInt16]1)             # colour planes
    $w.Write([UInt16]32)            # bits per pixel
    $w.Write([UInt32]$e.Bytes.Length)
    $w.Write([UInt32]$offset)
    $offset += $e.Bytes.Length
}
foreach ($e in $entries) { $w.Write($e.Bytes) }
$w.Flush()
[System.IO.File]::WriteAllBytes($icoPath, $out.ToArray())
$w.Dispose()
Write-Host "Wrote $icoPath ($($entries.Count) sizes: $($sizes -join ', '))"
