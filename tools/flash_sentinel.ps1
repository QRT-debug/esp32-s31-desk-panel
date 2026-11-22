# Flash sentinel: wait for COM5, flash, capture 25s boot log. ASCII only.
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\Admin\Desktop\sample_project"
. ".\idf_env.ps1"

$deadline = (Get-Date).AddMinutes(40)
$ready = $false
while (-not $ready -and (Get-Date) -lt $deadline) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports -contains "COM5") {
        Start-Sleep -Seconds 3
        $ports2 = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($ports2 -contains "COM5") { $ready = $true }
    } else {
        Start-Sleep -Seconds 1
    }
}
if (-not $ready) { Write-Output "SENTINEL=TIMEOUT"; exit }
Write-Output ("PORT_READY " + (Get-Date -Format "HH:mm:ss"))

$ok = $false
for ($i = 1; $i -le 2; $i++) {
    python "D:\esp\.espressif\v6.1\esp-idf\tools\idf.py" -p COM5 flash *> flash.log
    if ($LASTEXITCODE -eq 0) { $ok = $true; break }
    Start-Sleep -Seconds 5
}
if (-not $ok) {
    Write-Output "FLASH=1"
    Get-Content flash.log -Encoding utf8 | Select-String "error|A fatal" | Select-Object -First 5
    exit
}
Write-Output ("FLASH=0 " + (Get-Date -Format "HH:mm:ss"))
Write-Output "=================================================================="
Write-Output "  *** RGB PANEL NOTICE *******************************************"
Write-Output "  The LCD (HX8264) has NO reset pin and NO command channel -- it"
Write-Output "  can ONLY be reset by cutting power. A chip-only reset (esptool"
Write-Output "  hard-reset) leaves the panel in its previous scan state, which"
Write-Output "  shows up as edge flicker / partial corruption."
Write-Output "  => After flashing, if the screen flickers: UNPLUG USB 2s, replug."
Write-Output "  Normal use (cold boot) is NOT affected -- this is debug-only."
Write-Output "=================================================================="

# Capture 25s of boot/run log after esptool hard reset
try {
    $sp = New-Object System.IO.Ports.SerialPort("COM5", 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.Open()
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buf = ""
    while ($sw.Elapsed.TotalSeconds -lt 25) {
        $buf += $sp.ReadExisting()
        Start-Sleep -Milliseconds 150
    }
    $sp.Close()
    [System.IO.File]::WriteAllText("C:\Users\Admin\Desktop\sample_project\boot_capture.log", $buf)
    Write-Output "CAPTURE=DONE"
} catch {
    Write-Output ("CAPTURE_FAIL=" + $_.Exception.Message)
}
