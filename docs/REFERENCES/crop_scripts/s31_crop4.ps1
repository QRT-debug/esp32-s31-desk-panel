Add-Type -AssemblyName System.Drawing
$base = "C:\Users\Admin\Desktop\esp32-s3-eink-hardware\Temp\s31png"
$src = Join-Path $base "page-0003.png"
$img = [System.Drawing.Image]::FromFile($src)
Write-Output ("size = " + $img.Width + " x " + $img.Height)

function CropScale($img, $x0, $y0, $x1, $y1, $scale, $out) {
    $rx = [int]($x0 * $img.Width); $ry = [int]($y0 * $img.Height)
    $rw = [int](($x1 - $x0) * $img.Width); $rh = [int](($y1 - $y0) * $img.Height)
    $ow = [int]($rw * $scale); $oh = [int]($rh * $scale)
    $rect = New-Object System.Drawing.Rectangle($rx, $ry, $rw, $rh)
    $bmp = New-Object System.Drawing.Bitmap($ow, $oh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($img, (New-Object System.Drawing.Rectangle(0, 0, $ow, $oh)), $rect, [System.Drawing.GraphicsUnit]::Pixel)
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output ("saved " + $out + "  (" + $ow + "x" + $oh + ")")
}

CropScale $img 0.10 0.05 0.40 0.45 3.2 (Join-Path $base "a_codec.png")
CropScale $img 0.10 0.42 0.42 0.90 3.0 (Join-Path $base "a_mic.png")
CropScale $img 0.50 0.03 0.95 0.35 2.6 (Join-Path $base "a_pa.png")
$img.Dispose()
