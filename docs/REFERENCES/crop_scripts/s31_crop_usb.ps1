Add-Type -AssemblyName System.Drawing
$base = "C:\Users\Admin\Desktop\esp32-s3-eink-hardware\Temp\s31png"
$img = [System.Drawing.Image]::FromFile((Join-Path $base "page-0004.png"))
Write-Output ("page size = " + $img.Width + " x " + $img.Height)

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
    Write-Output ("saved " + (Split-Path $out -Leaf) + "  " + $ow + "x" + $oh)
}

CropScale $img 0.03 0.11 0.23 0.42 5.0 (Join-Path $base "u_com.png")
CropScale $img 0.73 0.11 0.93 0.40 5.0 (Join-Path $base "u_otg.png")
CropScale $img 0.66 0.55 0.90 0.86 5.0 (Join-Path $base "u_pwr.png")
Write-Output "done"
$img.Dispose()
