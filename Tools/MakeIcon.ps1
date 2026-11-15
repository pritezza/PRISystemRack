# Draw the PRISystemRack app icon (a small rack: three units with a status LED and a level meter) at a given size.
# Sizes of 32 px or less are drawn simplified (no meters, thicker and brighter units) so that they stay readable in the taskbar.
# Usage: powershell -File Tools/MakeIcon.ps1 -OutDir Resources/Icons [-Sizes 256,48,32,16] [-Preview preview.png]
param([string]$OutDir, [int[]]$Sizes = @(256, 48, 32, 16), [string]$Preview = "")
Add-Type -AssemblyName System.Drawing

function New-RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function C([string]$hex) { return [System.Drawing.ColorTranslator]::FromHtml($hex) }

function Draw-Icon([int]$size) {
    $simple = $size -le 32
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 256.0

    # background: rounded square, dark gradient, light border
    $inset = if ($simple) { 0.5 } else { 8 * $s }
    $bg = New-RoundedRect $inset $inset ($size - 2 * $inset) ($size - 2 * $inset) (48 * $s)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush ((New-Object System.Drawing.PointF 0, 0)), ((New-Object System.Drawing.PointF 0, $size)), (C '#3d424b'), (C '#17191c')
    $g.FillPath($brush, $bg)
    $borderColour = if ($simple) { '#a3a9b3' } else { '#737a86' }
    $pen = New-Object System.Drawing.Pen ((C $borderColour)), ([Math]::Max(1.0, 5 * $s))
    $g.DrawPath($pen, $bg)

    # three rack units
    $leds = @('#8fe3aa', '#5ccbfa', '#9584e0')
    $meters = @(0.78, 0.55, 0.9)
    if ($simple) { $unitH = 62 * $s; $gap = 10 * $s; $left = 26 * $s; $right = 230 * $s }
    else         { $unitH = 46 * $s; $gap = 14 * $s; $left = 40 * $s; $right = 216 * $s }
    $top = ($size - (3 * $unitH + 2 * $gap)) / 2
    for ($i = 0; $i -lt 3; $i++) {
        $y = $top + $i * ($unitH + $gap)
        $unit = New-RoundedRect $left $y ($right - $left) $unitH (10 * $s)
        $unitFill = if ($simple) { '#5d6470' } else { '#4a505b' }
        $g.FillPath((New-Object System.Drawing.SolidBrush (C $unitFill)), $unit)
        if (-not $simple) {
            $unitPen = New-Object System.Drawing.Pen ((C '#8d929c')), (2.5 * $s)
            $g.DrawPath($unitPen, $unit)
        }

        # status LED
        $ledR = if ($simple) { 19 * $s } else { 11 * $s }
        $cx = $left + 30 * $s; $cy = $y + $unitH / 2
        $g.FillEllipse((New-Object System.Drawing.SolidBrush (C $leds[$i])), ($cx - $ledR), ($cy - $ledR), (2 * $ledR), (2 * $ledR))

        if ($simple) {
            # one short light bar instead of a meter
            $bx = $left + 58 * $s; $bw = ($right - 14 * $s) - $bx; $bh = 20 * $s
            $g.FillRectangle((New-Object System.Drawing.SolidBrush (C '#d6dae0')), $bx, ($cy - $bh / 2), ($bw * $meters[$i]), $bh)
        } else {
            # level meter: dark track with a filled part
            $mx = $left + 58 * $s; $mw = ($right - 18 * $s) - $mx; $mh = 12 * $s
            $track = New-RoundedRect $mx ($cy - $mh / 2) $mw $mh (4 * $s)
            $g.FillPath((New-Object System.Drawing.SolidBrush (C '#141518')), $track)
            $fill = New-RoundedRect $mx ($cy - $mh / 2) ($mw * $meters[$i]) $mh (4 * $s)
            $g.FillPath((New-Object System.Drawing.SolidBrush (C '#5ccbfa')), $fill)
        }
    }
    $g.Dispose()
    return $bmp
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$images = @{}
foreach ($size in $Sizes) {
    $bmp = Draw-Icon $size
    $path = Join-Path $OutDir ("icon-{0}.png" -f $size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $images[$size] = $bmp
}

# preview sheet: each size at 1x and enlarged (nearest neighbour) on a light and a dark background
if ($Preview -ne "") {
    $sheet = New-Object System.Drawing.Bitmap 900, 330
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.InterpolationMode = 'NearestNeighbor'
    $g.FillRectangle((New-Object System.Drawing.SolidBrush (C '#f0f0f0')), 0, 0, 900, 165)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush (C '#202020')), 0, 165, 900, 165)
    foreach ($row in 0, 1) {
        $x = 10
        foreach ($size in $Sizes) {
            $img = $images[$size]
            $y = 10 + $row * 165
            $g.DrawImage($img, $x, $y, $size, $size)
            if ($size -le 48) { $g.DrawImage($img, $x, $y + 60, $size * 2, $size * 2) }
            $x += [Math]::Max($size, 96) + 20
        }
    }
    $sheet.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png)
}
