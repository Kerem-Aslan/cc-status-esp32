<#
.SYNOPSIS
  Tests the ESP32 status light step by step, using the same bodies Claude Code sends.

.EXAMPLE
  .\tools\Test-Light.ps1 -Ip 192.168.1.50
#>
param(
  [Parameter(Mandatory, Position = 0)]
  [string]$Ip
)

$ErrorActionPreference = 'Stop'
$hookUrl = "http://$Ip/claude-hook"

function Send-Hook([hashtable]$body) {
  $body.session_id = 'test-session'
  $json = $body | ConvertTo-Json -Compress
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $resp = Invoke-WebRequest -Method Post -Uri $hookUrl -ContentType 'application/json' -Body $json -TimeoutSec 2 -UseBasicParsing
  $sw.Stop()
  Write-Host ("  -> HTTP {0}, body {1} bytes, {2} ms" -f $resp.StatusCode, $resp.RawContentLength, $sw.ElapsedMilliseconds)
}

function Step([string]$title, [string]$expect, [hashtable]$body) {
  Write-Host ""
  Write-Host $title -ForegroundColor Cyan
  Send-Hook $body
  Write-Host "  Expected: $expect" -ForegroundColor Yellow
  Read-Host "  Press Enter if correct" | Out-Null
}

Write-Host "Checking status page: http://$Ip/status"
$s = Invoke-RestMethod -Uri "http://$Ip/status" -TimeoutSec 3
Write-Host "  Current state: $($s.state), IP: $($s.ip)" -ForegroundColor Green

Step 'UserPromptSubmit'            'Red, slow breathing'                  @{ hook_event_name = 'UserPromptSubmit'; prompt = 'hello' }
Step 'Notification (permission)'   'Yellow, clearly blinking'             @{ hook_event_name = 'Notification'; notification_type = 'permission_prompt'; message = 'Claude needs your permission' }
Step 'PostToolUse'                 'Back to red breathing'                @{ hook_event_name = 'PostToolUse'; tool_name = 'Bash'; tool_input = @{ command = 'ls' } }
Step 'PreToolUse (question)'       'Yellow, clearly blinking'             @{ hook_event_name = 'PreToolUse'; tool_name = 'AskUserQuestion' }
Step 'PreToolUse (other tool)'     'NO CHANGE, stays yellow'              @{ hook_event_name = 'PreToolUse'; tool_name = 'Bash' }
Step 'Stop'                        'Solid green'                          @{ hook_event_name = 'Stop'; last_assistant_message = 'Done.' }
Step 'Notification (idle_prompt)'  'NO CHANGE, stays green'               @{ hook_event_name = 'Notification'; notification_type = 'idle_prompt' }
Step 'Unknown event'               'NO CHANGE, stays green'               @{ hook_event_name = 'SomethingNew' }
Step 'StopFailure'                 'All three blinking fast'              @{ hook_event_name = 'StopFailure'; error = 'rate_limit' }

# Real PostToolUse bodies can be large (e.g. the whole file that was read).
Write-Host ""
Write-Host "Large body test (~300 KB PostToolUse)" -ForegroundColor Cyan
Send-Hook @{ hook_event_name = 'PostToolUse'; tool_name = 'Read'; tool_response = ('x' * 300000) }
Write-Host "  Expected: red breathing, request well under 2 s" -ForegroundColor Yellow
Read-Host "  Press Enter if correct" | Out-Null

Step 'SessionEnd'                  'All off'                              @{ hook_event_name = 'SessionEnd'; reason = 'other' }

Write-Host ""
Write-Host "Test finished. To install the hooks: .\tools\Install-Hooks.ps1 -Ip $Ip" -ForegroundColor Green
