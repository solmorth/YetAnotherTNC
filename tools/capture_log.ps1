# Usage: powershell -File capture_log.ps1 -Port COM5 -LogFile crash.log
# Ctrl+C to stop. Reopens the port automatically if it drops out
# (e.g. the board resets and the USB CDC device re-enumerates),
# so you don't miss a fault dump printed right before/after a reset.
param(
    [Parameter(Mandatory=$true)][string]$Port,
    [string]$LogFile = "crash.log",
    [int]$Baud = 115200
)

while ($true) {
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, "None", 8, "One"
        $sp.ReadTimeout = 500
        $sp.Open()
        Write-Host "[capture] opened $Port, logging to $LogFile"

        while ($sp.IsOpen) {
            try {
                $line = $sp.ReadLine()
                $stamped = "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $line
                Write-Host $stamped
                Add-Content -Path $LogFile -Value $stamped
            } catch [System.TimeoutException] {
                # no data this tick, keep polling
            }
        }
    } catch {
        Write-Host "[capture] $Port unavailable, retrying in 1s ($($_.Exception.Message))"
        Start-Sleep -Seconds 1
    }
}
