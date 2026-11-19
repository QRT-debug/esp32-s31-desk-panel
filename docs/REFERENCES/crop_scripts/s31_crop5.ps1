Add-Type -AssemblyName System.Drawing
$base = "C:\Users\Admin\Desktop\esp32-s3-eink-hardware\Temp\s31png"
$src = Join-Path $base "page-0003.png"
$img = [System.Drawing.Image]::FromFile($src)

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

# 左侧 I2S 网络名 + 引脚 11/10/9/4/3/2 区域
CropScale $img 0.10 0.24 0.32 0.46 6.0 (Join-Path $base "a_i2s_left.png")
# 右侧 MIC/LOUT 网络名
CropScale $img 0.28 0.24 0.42 0.44 6.0 (Join-Path $base "a_right_nets.png")
$img.Dispose()
