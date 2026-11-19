# Live monitor for live.log (byte-offset version)
# Usage: double-click watch_live.bat. Ctrl+C to quit.
#
# Why not Get-Content -Wait: on Windows, if the log is moved or recreated
# (which happens every time the agent starts a new task), the old handle keeps
# following the removed file object and the window silently freezes.
# This version tracks a byte offset and re-opens the file each tick, so it
# survives recreation / truncation / encoding changes.
#
# Output is always cut at the last newline, so a half-written line is shown
# on the next tick instead of being split in two.

$log = "C:\Users\Admin\Desktop\sample_project\live.log"

$enc = [System.Text.Encoding]::UTF8
$pos = 0
$bomDone = $false

Write-Host "============================================" -ForegroundColor Cyan
Write-Host " watching: $log" -ForegroundColor Cyan
Write-Host " handles log recreation / truncation" -ForegroundColor DarkGray
Write-Host " Ctrl+C to quit" -ForegroundColor DarkGray
Write-Host "============================================" -ForegroundColor Cyan

while ($true) {
    Start-Sleep -Milliseconds 400
    if (-not (Test-Path $log)) { continue }

    $bytes = $null
    try {
        $bytes = [System.IO.File]::ReadAllBytes($log)
    }
    catch {
        continue   # writer holds the file for a moment: retry next tick
    }
    if ($null -eq $bytes) { continue }

    # first successful read: detect BOM / encoding, set the starting offset
    if (-not $bomDone) {
        if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
            $enc = [System.Text.Encoding]::Unicode
            $pos = 2
        }
        elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            $enc = [System.Text.Encoding]::UTF8
            $pos = 3
        }
        else {
            $enc = [System.Text.Encoding]::UTF8
            $pos = 0
        }
        $bomDone = $true
    }

    # file shrank => recreated by a new task
    if ($bytes.Length -lt $pos) {
        Write-Host ""
        Write-Host "--- log recreated (new task) ---" -ForegroundColor Yellow
        Write-Host ""
        $pos = 0
    }

    if ($bytes.Length -gt $pos) {
        # cut at the last newline so a partial line is never printed
        $end = $bytes.Length - 1
        while ($end -ge $pos -and $bytes[$end] -ne 10) { $end-- }
        if ($end -ge $pos) {
            $count = $end - $pos + 1
            $chunk = $enc.GetString($bytes, $pos, $count)
            Write-Host $chunk -NoNewline
            $pos = $end + 1
        }
    }
}
