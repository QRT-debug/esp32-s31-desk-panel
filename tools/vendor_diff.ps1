# Flash vendor firmware via idf.py, capture register dump, reflash ours. ASCII only.
$ErrorActionPreference = "Continue"
. "C:\Users\Admin\Desktop\sample_project\idf_env.ps1"
$env:IDF_COMPONENT_REGISTRY_URL = "https://components.espressif.cn"

Write-Output "STEP1: flash vendor firmware"
Set-Location "D:\esp_projects\20_recoding"
python "D:\esp\.espressif\v6.1\esp-idf\tools\idf.py" -p COM5 flash *> vendor_flash.log
Write-Output ("VENDOR_FLASH=" + $LASTEXITCODE)
if ($LASTEXITCODE -ne 0) { Get-Content vendor_flash.log -Encoding utf8 | Select-String "error|fatal|FAILED" | Select-Object -First 8; exit }

Start-Sleep -Seconds 2
Write-Output "STEP2: capture 30s vendor log"
try {
    $sp = New-Object System.IO.Ports.SerialPort("COM5", 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.Open()
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buf = ""
    while ($sw.Elapsed.TotalSeconds -lt 75) {
        $buf += $sp.ReadExisting()
        Start-Sleep -Milliseconds 150
    }
    $sp.Close()
    [System.IO.File]::WriteAllText("D:\esp_projects\20_recoding\build\vendor_dump.log", $buf)
    Write-Output "VENDOR_CAPTURE=DONE"
} catch {
    Write-Output ("VENDOR_CAPTURE_FAIL=" + $_.Exception.Message)
}

Write-Output "STEP3: reflash our firmware"
Set-Location "C:\Users\Admin\Desktop\sample_project"
python "D:\esp\.espressif\v6.1\esp-idf\tools\idf.py" -p COM5 flash *> reflash.log
Write-Output ("REFLASH=" + $LASTEXITCODE)
