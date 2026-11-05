<#
.SYNOPSIS
  Uploads firmware to the ESP32 over WiFi (over-the-air update, OTA).

.DESCRIPTION
  Reads the password from the OTA_PASSWORD line in include/secrets.h and sends firmware.bin
  to http://<ip>/update, then waits for the device to come back up.
  Normally you don't call this directly:  pio run -e ota -t upload

.EXAMPLE
  .\tools\Flash-Ota.ps1 -Ip 192.168.1.50
#>
param(
  [Parameter(Mandatory)][string]$Ip,
  [string]$Bin = (Join-Path $PSScriptRoot '..\.pio\build\ota\firmware.bin')
)

$ErrorActionPreference = 'Stop'
$secrets = Join-Path $PSScriptRoot '..\include\secrets.h'

if (-not (Test-Path -LiteralPath $Bin)) { throw "Firmware not found: $Bin  (build first: pio run -e ota)" }
$m = Select-String -LiteralPath $secrets -Pattern '^\s*#define\s+OTA_PASSWORD\s+"([^"]*)"' | Select-Object -First 1
if (-not $m) { throw "No OTA_PASSWORD line in include\secrets.h." }
$password = $m.Matches[0].Groups[1].Value

# Grab the device uptime before uploading; a lower value afterwards means it restarted.
try { $before = Invoke-RestMethod "http://$Ip/status" -TimeoutSec 5 }
catch { throw "Can't reach the ESP32 (http://$Ip/status). Is it powered on and on the same network?" }

$size = [math]::Round((Get-Item -LiteralPath $Bin).Length / 1KB)
Write-Host "Uploading: $size KB -> http://$Ip/update  (the light will blink yellow/green)"

# Pass the password through a temporary curl config file so it doesn't show up in the process list
$cfg = New-TemporaryFile
try {
  Set-Content -LiteralPath $cfg -Value "user = `"ota:$password`"" -Encoding ascii
  $out = & curl.exe -sS --fail-with-body --max-time 180 -K $cfg -F "firmware=@$Bin" "http://$Ip/update" 2>&1
  $code = $LASTEXITCODE
} finally {
  Remove-Item -LiteralPath $cfg -Force -ErrorAction SilentlyContinue
}
if ($code -ne 0) {
  if ("$out" -match '401') { throw "Password rejected. The firmware on the board may have been built with a different OTA_PASSWORD; flash once over USB." }
  throw "Upload failed (curl $code): $out"
}
Write-Host "$out".Trim()

# Wait for it to come back up (about 5-10 s)
Write-Host -NoNewline "Waiting for restart"
$deadline = (Get-Date).AddSeconds(45)
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 1000
  Write-Host -NoNewline "."
  try {
    $now = Invoke-RestMethod "http://$Ip/status" -TimeoutSec 2
    if ($now.uptime_s -lt $before.uptime_s -or $now.uptime_s -lt 30) {
      Write-Host ""
      Write-Host "Done: new firmware is running (uptime $($now.uptime_s) s, state: $($now.state))." -ForegroundColor Green
      exit 0
    }
  } catch { }
}
Write-Host ""
Write-Warning "The device did not come back within 45 s. Check the serial monitor or reset the board."
exit 1
