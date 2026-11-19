# Detached build runner: survives session idle (ASCII only)
$ErrorActionPreference = "Continue"
Set-Location "C:\Users\Admin\Desktop\sample_project"
. ".\idf_env.ps1"
python "D:\esp\.espressif\v6.1\esp-idf\tools\idf.py" build *> "C:\Users\Admin\Desktop\sample_project\live.log"
Write-Output ("BUILD=" + $LASTEXITCODE)
if ($LASTEXITCODE -eq 0) {
    powershell -ExecutionPolicy Bypass -File ".\tools\flash_sentinel.ps1"
} else {
    Get-Content "C:\Users\Admin\Desktop\sample_project\live.log" -Encoding utf8 | Select-String "error:" | Select-Object -First 5
}
