# make_icon.ps1 — generates the SANAD application icon as a multi-resolution .ico.
#
# Why a script and not a checked-in binary: the .ico is a BUILD ARTIFACT of the
# brand, and keeping the only copy in the repository means nobody can change the
# logo without a hex editor. This draws it from the same geometry and palette the
# launcher uses (see draw_vec_icon in Editor/src/ProjectLauncher.cpp), so the
# taskbar icon and the in-app brand mark are the same product by construction.
#
# The icon is a 3D cube in isometric projection on a dark rounded tile, with a
# green vertex dot on the near corner — the gizmo origin, in the accent green the
# UI already uses. Chosen because: (a) the engine's job is 3D, (b) it reads at
# 16 px where a wordmark cannot, and (c) it matches the launcher's palette so the
# title bar and the app header look like one product.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$outDir = Join-Path $PSScriptRoot 'Editor\resources'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$icoPath = Join-Path $outDir 'SANAD.ico'

# Sizes Windows actually asks for. 256 is the important one: it is also the
# PNG-compressed entry Vista+ prefers.
$sizes = @(16, 24, 32, 48, 64, 128, 256)

# PowerShell's `New-Object Type(a, b)` short form mis-parses arithmetic inside the
# argument list, so points go through this helper instead. Every coordinate is
# an explicit [float] because $big/$u are doubles and PointF takes floats.
function P([double]$x, [double]$y) {
    return [System.Drawing.PointF]::new([single]$x, [single]$y)
}
function Brush([int]$a, [int]$r, [int]$g, [int]$b) {
    return [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb($a, $r, $g, $b))
}

function New-LogoBitmap([int]$size) {
    # Supersample 4x and let the bitmap's own downscaling do the anti-aliasing.
    # Drawing at 16 px directly gives jagged diagonals on the cube.
    $ss = 4
    $big = $size * $ss
    $bmp = [System.Drawing.Bitmap]::new($big, $big,
           [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    [double]$u = $big / 256.0   # one design unit = 1/256 of the tile

    # --- Tile: a rounded dark square, inset so the shape has a margin.
    [double]$inset = 14.0 * $u
    [double]$rad = 46.0 * $u
    [double]$tl = $inset
    [double]$tt = $inset
    [double]$tr = $big - $inset
    [double]$tb = $big - $inset
    $tile = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $tile.AddArc([single]$tl, [single]$tt, [single]$rad, [single]$rad, 180, 90)
    $tile.AddArc([single]($tr - $rad), [single]$tt, [single]$rad, [single]$rad, 270, 90)
    $tile.AddArc([single]($tr - $rad), [single]($tb - $rad), [single]$rad, [single]$rad, 0, 90)
    $tile.AddArc([single]$tl, [single]($tb - $rad), [single]$rad, [single]$rad, 90, 90)
    $tile.CloseFigure()
    $p0 = P (40.0 * $u) (40.0 * $u)
    $p1 = P (216.0 * $u) (216.0 * $u)
    $bg = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        $p0, $p1,
        [System.Drawing.Color]::FromArgb(255, 58, 62, 74),
        [System.Drawing.Color]::FromArgb(255, 24, 26, 32))
    $g.FillPath($bg, $tile)
    $tilePen = [System.Drawing.Pen]::new(
        [System.Drawing.Color]::FromArgb(255, 86, 92, 108), [single][math]::Max(1.0, 5.0 * $u))
    $g.DrawPath($tilePen, $tile)

    # --- Cube: isometric. Top face lightest, left face mid, right face dark, so
    # the light reads as coming from the upper left like the 3D viewport does.
    [double]$cx = 128.0 * $u
    [double]$cy = 132.0 * $u
    [double]$hw = 74.0 * $u     # half width of the top face
    [double]$hh = 40.0 * $u     # half depth (vertical squash of the top face)
    [double]$vh = 48.0 * $u     # side height

    $top = [System.Drawing.PointF[]]@(
        (P $cx ($cy - $hh)), (P ($cx + $hw) $cy), (P $cx ($cy + $hh)), (P ($cx - $hw) $cy))
    $left = [System.Drawing.PointF[]]@(
        (P ($cx - $hw) $cy), (P $cx ($cy + $hh)),
        (P $cx ($cy + $hh + $vh)), (P ($cx - $hw) ($cy + $vh)))
    $right = [System.Drawing.PointF[]]@(
        (P $cx ($cy + $hh)), (P ($cx + $hw) $cy),
        (P ($cx + $hw) ($cy + $vh)), (P $cx ($cy + $hh + $vh)))

    $bTop = Brush 255 130 214 148
    $bLeft = Brush 255 56 128 96
    $bRight = Brush 255 38 98 74
    $g.FillPolygon($bTop, $top)
    $g.FillPolygon($bLeft, $left)
    $g.FillPolygon($bRight, $right)
    # The edge pass is what stops the cube collapsing into a flat hexagon at
    # 16 px: a lighter 1-unit stroke on the three silhouettes keeps the facets
    # separated once there is barely a pixel between them.
    $edge = [System.Drawing.Pen]::new(
        [System.Drawing.Color]::FromArgb(200, 232, 255, 238),
        [single][math]::Max(0.9, 3.0 * $u))
    $edge.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPolygon($edge, $top)
    $g.DrawPolygon($edge, $left)
    $g.DrawPolygon($edge, $right)

    # --- Vertex dot: the gizmo origin, in the accent green the UI uses. This one
    # detail is what makes the icon say "editor" rather than "a cube".
    [double]$dr = 16.0 * $u
    $bGlow = Brush 64 70 210 120
    $bDot = Brush 255 70 210 120
    $g.FillEllipse($bGlow, [single]($cx - $dr * 2.0), [single]($cy - $dr * 2.0),
                   [single]($dr * 4.0), [single]($dr * 4.0))
    $g.FillEllipse($bDot, [single]($cx - $dr), [single]($cy - $dr),
                   [single]($dr * 2.0), [single]($dr * 2.0))

    $g.Dispose()
    $bGlow.Dispose(); $bDot.Dispose(); $bTop.Dispose(); $bLeft.Dispose(); $bRight.Dispose()
    $edge.Dispose(); $tilePen.Dispose(); $bg.Dispose(); $tile.Dispose()

    $small = [System.Drawing.Bitmap]::new($size, $size,
                  [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g2 = [System.Drawing.Graphics]::FromImage($small)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g2.Clear([System.Drawing.Color]::Transparent)
    $g2.DrawImage($bmp, 0, 0, $size, $size)
    $g2.Dispose()
    $bmp.Dispose()
    return $small
}

# --- Render every size, keeping the PNG bytes --------------------------------
$pngs = @()
foreach ($s in $sizes) {
    $b = New-LogoBitmap $s
    $ms = [System.IO.MemoryStream]::new()
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , @{ size = $s; bytes = $ms.ToArray() }
    if ($s -eq 256) {
        # Also drop a full-size PNG next to the .ico: that is what a README badge
        # or a docs page links, and generating it here means it can never be a
        # different logo from the one inside the exe.
        $b.Save((Join-Path $outDir 'SANAD.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $ms.Dispose(); $b.Dispose()
    Write-Host "  rendered ${s}x${s}"
}

# --- Pack the .ico -----------------------------------------------------------
# ICONDIR:      reserved(2)=0, type(2)=1 (icon), count(2)
# ICONDIRENTRY: w, h (1 byte each; 0 means 256), palette size=0, reserved=0,
#               planes(2)=1, bitCount(2)=32, bytesInRes(4), imageOffset(4)
# The image data is the raw PNG, which Windows has accepted since Vista. Storing
# PNG rather than a DIB is what keeps the 256x256 entry from being a 256 KB
# uncompressed blob inside the exe.
$dir = [System.IO.MemoryStream]::new()
$bw = [System.IO.BinaryWriter]::new($dir)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $w = $p.size; if ($w -ge 256) { $w = 0 }
    $h = $p.size; if ($h -ge 256) { $h = 0 }
    $bw.Write([byte]$w)
    $bw.Write([byte]$h)
    $bw.Write([byte]0)     # palette size
    $bw.Write([byte]0)     # reserved
    $bw.Write([uint16]1)   # planes
    $bw.Write([uint16]32)  # bit count
    $bw.Write([uint32]$p.bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $p.bytes.Length
}
foreach ($p in $pngs) { $bw.Write($p.bytes) }
$bw.Flush()
[System.IO.File]::WriteAllBytes($icoPath, $dir.ToArray())
$bw.Dispose(); $dir.Dispose()

$kb = [math]::Round((Get-Item $icoPath).Length / 1KB, 1)
Write-Host ""
Write-Host "wrote $icoPath ($kb KB, $($pngs.Count) resolutions)"
Write-Host "wrote $(Join-Path $outDir 'SANAD.png')"
