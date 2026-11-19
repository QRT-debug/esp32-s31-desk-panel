Add-Type -AssemblyName System.Drawing
$base = "C:\Users\Admin\Desktop\esp32-s3-eink-hardware\Temp\s31png"
$img = [System.Drawing.Image]::FromFile((Join-Path $base "page-0003.png"))

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

CropScale $img 0.00 0.29 0.17 0.45 3.4 (Join-Path $base "c_mclk.png")
Write-Output "done"
$img.Dispose()
