<#
.SYNOPSIS
  Adds or removes the Claude Code status light hooks in ~/.claude/settings.json.

.DESCRIPTION
  Never overwrites your existing settings: the file is read, only this project's entries
  are added under "hooks", and a backup is made before saving. This project's entries are
  recognized by "/claude-hook" in their URL or command. Running the script again replaces
  the old entries with new ones, so just rerun it when the IP changes.

  Each hook is a one-line command that runs curl.exe and sends ONLY the event name, e.g.
  POST http://<ip>/claude-hook?event=Stop. The hook's JSON input (your prompt, file
  contents, command output) is never sent over the network.

.EXAMPLE
  .\tools\Install-Hooks.ps1 -Ip 192.168.1.50
.EXAMPLE
  .\tools\Install-Hooks.ps1 -Uninstall
.EXAMPLE
  .\tools\Install-Hooks.ps1 -Ip 192.168.1.50 -WhatIf     # Show the result without writing
#>
[CmdletBinding(SupportsShouldProcess, DefaultParameterSetName = 'Install')]
param(
  # ESP32 IP address (optionally with a port: 192.168.1.50:8080)
  [Parameter(Mandatory, ParameterSetName = 'Install', Position = 0)]
  [ValidatePattern('^[A-Za-z0-9.\-]+(:\d{1,5})?$')]
  [string]$Ip,

  [Parameter(Mandatory, ParameterSetName = 'Uninstall')]
  [switch]$Uninstall,

  [string]$SettingsPath = (Join-Path $HOME '.claude\settings.json'),

  # Seconds curl waits for the ESP32. When the ESP32 is off, each hook waits about this long.
  [Parameter(ParameterSetName = 'Install')]
  [ValidateRange(1, 30)]
  [int]$Timeout = 2,

  # Skip the reachability check before installing
  [Parameter(ParameterSetName = 'Install')]
  [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'
$HookPath = '/claude-hook'

# Event -> matcher ($null = no matcher, always fires)
# idle_prompt is left out on purpose: it fires a while after a task ends and would turn green into yellow.
$Events = [ordered]@{
  UserPromptSubmit   = $null
  PostToolUse        = $null
  PostToolUseFailure = $null
  Notification       = 'permission_prompt|elicitation_dialog'
  # Claude asks you a question (AskUserQuestion) or waits for plan approval (ExitPlanMode)
  PreToolUse         = 'AskUserQuestion|ExitPlanMode'
  Stop               = $null
  StopFailure        = $null
  SessionEnd         = $null
}

function Test-IsOurHook($hook) {
  # Current form: command hooks running curl.exe. Older form: type "http" hooks.
  return (($hook.type -eq 'command') -and ("$($hook.command)" -like "*$HookPath`?event=*")) -or
         (($hook.type -eq 'http') -and ("$($hook.url)" -like "*$HookPath"))
}

# One-line hook command. It works in both shells Claude Code uses on Windows (Git Bash, or
# PowerShell when Git Bash isn't installed). "; exit 0" keeps Claude Code from showing a
# "hook error" on every tool call while the ESP32 is off.
function New-HookCommand([string]$eventName) {
  $max = $Timeout + 1
  return "curl.exe -s --connect-timeout $Timeout -m $max -X POST `"http://$Ip$HookPath`?event=$eventName`"; exit 0"
}

# Removes this project's entries under hooks, and drops groups/events that become empty.
function Remove-OurHooks($settings) {
  $removed = 0
  $hooksProp = $settings.PSObject.Properties['hooks']
  if (-not $hooksProp -or $null -eq $hooksProp.Value) { return 0 }
  $hooks = $hooksProp.Value

  foreach ($evProp in @($hooks.PSObject.Properties)) {
    $keptGroups = @()
    foreach ($group in @($evProp.Value)) {
      $inner = @($group.hooks)
      $keptInner = @($inner | Where-Object { -not (Test-IsOurHook $_) })
      $removed += $inner.Count - $keptInner.Count
      if ($keptInner.Count -eq $inner.Count) {
        $keptGroups += $group                 # Nothing of ours here, keep as-is
      } elseif ($keptInner.Count -gt 0) {
        $group.hooks = $keptInner             # Keep other hooks that share the group
        $keptGroups += $group
      }
    }
    if ($keptGroups.Count -gt 0) {
      $hooks.($evProp.Name) = $keptGroups
    } else {
      $hooks.PSObject.Properties.Remove($evProp.Name)
    }
  }
  if (@($hooks.PSObject.Properties).Count -eq 0) {
    $settings.PSObject.Properties.Remove('hooks')
  }
  return $removed
}

# --- Read settings ---
if (Test-Path -LiteralPath $SettingsPath) {
  $raw = [IO.File]::ReadAllText($SettingsPath)
  $settings = if ($raw.Trim()) { $raw | ConvertFrom-Json } else { [pscustomobject]@{} }
} else {
  if ($Uninstall) { Write-Host "No settings file, nothing to remove: $SettingsPath"; return }
  $settings = [pscustomobject]@{}
}

$removed = Remove-OurHooks $settings

if (-not $Uninstall) {
  $url = "http://$Ip$HookPath"

  if (-not $SkipCheck) {
    try {
      $status = Invoke-RestMethod -Uri "http://$Ip/status" -TimeoutSec 3
      Write-Host "ESP32 is reachable. Current state: $($status.state)" -ForegroundColor Green
    } catch {
      Write-Warning "http://$Ip/status did not respond ($($_.Exception.Message))."
      Write-Warning "Add -SkipCheck to install anyway. Try the manual tests in the README first."
      return
    }
  }

  if (-not $settings.PSObject.Properties['hooks']) {
    $settings | Add-Member -NotePropertyName hooks -NotePropertyValue ([pscustomobject]@{})
  }
  $hooks = $settings.hooks

  foreach ($name in $Events.Keys) {
    $group = [ordered]@{}
    if ($Events[$name]) { $group.matcher = $Events[$name] }
    # Claude Code's own timeout is a safety net a few seconds above curl's.
    $group.hooks = @([pscustomobject][ordered]@{ type = 'command'; command = (New-HookCommand $name); timeout = $Timeout + 3 })
    $groupObj = [pscustomobject]$group

    if ($hooks.PSObject.Properties[$name]) {
      $hooks.$name = @($hooks.$name) + $groupObj   # Add next to existing hooks
    } else {
      $hooks | Add-Member -NotePropertyName $name -NotePropertyValue @($groupObj)
    }
  }
}

$json = ConvertTo-Json -InputObject $settings -Depth 64

if ($PSCmdlet.ShouldProcess($SettingsPath, $(if ($Uninstall) { 'Remove hooks' } else { 'Add hooks' }))) {
  $dir = Split-Path -Parent $SettingsPath
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
  if (Test-Path -LiteralPath $SettingsPath) {
    $backup = "$SettingsPath.bak-$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')"
    Copy-Item -LiteralPath $SettingsPath -Destination $backup
    Write-Host "Backup: $backup"
  }
  # UTF-8 without BOM
  [IO.File]::WriteAllText($SettingsPath, $json + [Environment]::NewLine, (New-Object Text.UTF8Encoding $false))

  if ($Uninstall) {
    Write-Host "$removed hook(s) removed." -ForegroundColor Green
  } else {
    if ($removed) { Write-Host "Removed $removed old entr$(if ($removed -eq 1) {'y'} else {'ies'})." }
    Write-Host "Hooks added for $($Events.Count) events -> $url`?event=... (timeout $Timeout s)" -ForegroundColor Green
    Write-Host "Only event names are sent; prompts and file contents stay on this PC."
    Write-Host "Claude Code watches the settings file, so running sessions pick this up too."
  }
} else {
  $json
}
