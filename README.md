# Claude Code Status Light (ESP32 + 3 LEDs)

A desk traffic light that shows what Claude Code is doing right now. On every state change a
Claude Code hook runs a one-line `curl` that tells the ESP32 the event name. No scripts, no serial
connection, nothing running in the background on the PC.

**Privacy:** only the event name (e.g. `Stop`) goes over the network. Claude Code hands each hook
a JSON payload with your prompt, file contents and command output; the installed hooks never send
it. See [What goes over the network](#what-goes-over-the-network).

| State | Hook event | LEDs |
|---|---|---|
| Agent working | `UserPromptSubmit`, `PostToolUse`, `PostToolUseFailure` | Red, slow breathing (gamma-corrected PWM) |
| Waiting for approval/input | `Notification` (`permission_prompt` / `elicitation_dialog`) | Yellow, clearly blinking |
| Claude asks you a question / plan approval | `PreToolUse` (`AskUserQuestion` / `ExitPlanMode`) | Yellow, clearly blinking |
| Done | `Stop` | Solid green |
| Error (rate limit, API error...) | `StopFailure` | All three blinking fast |
| Session ended | `SessionEnd` | All off |
| Probably interrupted | Red with no events for 20 min | Dim solid red |
| *(power-on)* | - | R, Y, G test in turn, then a WiFi waiting animation, two green flashes when connected |
| *(OTA update)* | - | Yellow/green alternating |

> `PostToolUseFailure` is there because when a tool you approved fails, Claude Code sends it
> instead of `PostToolUse`. Without it the light would stay yellow until the next event.

---

## 1. Wiring

Board: **ESP32-WROOM-32 DevKit V1** (the one with pins labeled `D25`, `D26`...).

```
                 ESP32 DevKit V1 - right header (USB at the bottom)
                 +----------+
                 | EN       |
                 | VP  D36  |
                 | VN  D39  |
                 | D34      |
                 | D35      |
                 | D32      |
                 | D33      |
  GREEN    <---- | D25      | --[220-330 ohm]-->|-- GND
  YELLOW   <---- | D26      | --[220-330 ohm]-->|-- GND
  RED      <---- | D27      | --[220-330 ohm]-->|-- GND
                 | D14      |
                 | D12      |
                 | D13      |
  common GND <-- | GND      |
                 | VIN      |
                 +--[USB]---+
```

| LED | Pin | Connection |
|---|---|---|
| Red | **D27** (GPIO27) | D27 -> resistor -> LED long leg (anode) - LED short leg (cathode) -> GND |
| Yellow | **D26** (GPIO26) | D26 -> resistor -> anode - cathode -> GND |
| Green | **D25** (GPIO25) | D25 -> resistor -> anode - cathode -> GND |

- **Always use a resistor.** Without one the LED pulls more current than the GPIO can safely
  supply. 220-330 ohm is ideal; anything from 100 ohm to 1 kohm works (higher = dimmer).
- All three cathodes can share one GND pin.
- **D2, D15, D5, D12 are not used.** They are strapping pins and affect the boot mode.
  D2 is also wired to the board's blue on-board LED.
- To change pins, edit only `include/config.h`.

---

## 2. Setup

### Requirements
- VS Code + the **PlatformIO** extension (or just the `pio` command line)
- USB driver for your board's USB chip (CP210x or CH340)
- Claude Code with HTTP hook support

### Steps
1. Enter your WiFi credentials:
   ```powershell
   Copy-Item include\secrets.example.h include\secrets.h
   notepad include\secrets.h
   ```
   Fill in `WIFI_SSID`, `WIFI_PASSWORD` and a long random `OTA_PASSWORD`.
   `secrets.h` is in `.gitignore`, so it never gets committed. The ESP32 only connects to
   **2.4 GHz** networks, and the PC and the ESP32 must be on the same network.
2. Put the board in download mode and flash it:
   - Hold **BOOT**, tap **EN**, release **BOOT**.
   - Then:
     ```powershell
     pio run -t upload
     pio device monitor
     ```
   (In VS Code the bottom bar's **Upload** and **Monitor** buttons do the same.)

   This project sets `board_upload.before_reset = no_reset` in `platformio.ini`. Normally esptool
   resets the board right before flashing, which kicks it back out of a download mode you entered
   by hand. If your board's auto-reset works (it flashes without pressing any buttons), you can
   delete that line.
3. The serial monitor should show something like:
   ```
   ==================================================
    WiFi connected: HomeNetwork  (RSSI -52 dBm)
    IP address : 192.168.1.50
    Hook URL   : http://192.168.1.50/claude-hook
    Status page: http://192.168.1.50/
    MAC        : 24:6F:28:AA:BB:CC  (for a DHCP reservation)
   ==================================================
   ```
   If you open the serial monitor late, the IP is reprinted every 30 seconds until the first
   request arrives.

If `pio` isn't recognized in PowerShell, it isn't on your PATH. Either use the PlatformIO terminal
in VS Code, or run it by its full path:
`& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload`

### Over-the-air updates (OTA)

Once OTA-capable firmware has been flashed over USB, you don't need the cable anymore:

```powershell
pio run -e ota -t upload
```

- The firmware is sent over WiFi to `http://<ip>/update`. The light blinks yellow/green while
  it's being written, then the board restarts on its own (~5-10 s).
- The password is the `OTA_PASSWORD` line in `include/secrets.h`; `tools/Flash-Ota.ps1` reads it
  from there. If you change the password, flash the new build **once over USB**: the firmware on
  the board still expects the old one.
- The target IP is the `custom_ota_ip` line under `[env:ota]` in `platformio.ini`.
- If an upload is interrupted, the board keeps running the old firmware. The new firmware only
  takes over once it has been fully written and verified.
- `pio run -t upload` (no environment) still flashes over USB.

This uses an HTTP upload to the ESP32's web server rather than ArduinoOTA, because ArduinoOTA needs
the ESP32 to connect back to the PC, which Windows Firewall often blocks.

### IP address: DHCP reservation or static IP?

Recommended: **a DHCP reservation on your router.**

- In the router's admin page (usually *LAN -> DHCP -> Address Reservation / Static Lease*) assign
  an IP to the **MAC address** shown on the serial monitor. The device shows up as
  `cc-status-light` in the router's list.
- The IP is managed in one place, can't clash with the DHCP pool, and you don't need to bake the
  gateway and DNS into the firmware.
- If you can't access the router, enable the `STATIC_IP` lines in `secrets.h` (all four). The IP
  you choose must be **outside** the router's DHCP pool.
- Don't rely on mDNS names like `cc-status-light.local` on Windows; they don't always resolve.
  Use the IP address in the hooks.

### Brightness

Drag the slider on the status page, or `Invoke-RestMethod "http://$ip/brightness?value=30"`.
The value (1-100 %) is perceived brightness, applied before gamma correction, so 50 % really looks
about half as bright. It's saved to the ESP32's flash about 2 seconds after you stop moving the
slider and survives power cycles. The default before the first change is 50 %
(`DEFAULT_BRIGHTNESS_PCT` in `config.h`).

---

## 3. Testing: by hand first, then with hooks

In the examples below, replace `$ip` with your ESP32's IP:

```powershell
$ip = '192.168.1.50'
```

### 3a. From a browser
Open `http://192.168.1.50/`. The page shows the current state, the last event, a
**brightness slider**, and a button for each state. The **LED test** button replays the power-on test, which is handy for checking
the wiring.

### 3b. Change state by hand from PowerShell
```powershell
Invoke-RestMethod "http://$ip/state?name=working"   # red breathing
Invoke-RestMethod "http://$ip/state?name=waiting"   # yellow blinking
Invoke-RestMethod "http://$ip/state?name=done"      # solid green
Invoke-RestMethod "http://$ip/state?name=error"     # all three blinking fast
Invoke-RestMethod "http://$ip/state?name=off"       # all off
Invoke-RestMethod "http://$ip/state?name=stale"     # dim red (no events for 20 min)
Invoke-RestMethod "http://$ip/state?name=test"      # LED test
Invoke-RestMethod "http://$ip/status"               # status (JSON)
Invoke-RestMethod "http://$ip/brightness?value=30"  # overall brightness 1-100 %
```

### 3c. Send hook events by hand
The hook endpoint returns **200 with an empty body**, so `Invoke-RestMethod` prints nothing.
That's expected. Watch the LEDs and the `[hook] ...` lines on the serial monitor.

This is exactly what the installed hooks send (event name only):

```powershell
Invoke-RestMethod -Method Post "http://$ip/claude-hook?event=UserPromptSubmit"   # red
Invoke-RestMethod -Method Post "http://$ip/claude-hook?event=Notification"       # yellow
Invoke-RestMethod -Method Post "http://$ip/claude-hook?event=Stop"               # green
```

The endpoint also accepts a full Claude Code JSON body, which lets you test the filtering rules:

```powershell
$u = "http://$ip/claude-hook"
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"UserPromptSubmit"}'
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"Notification","notification_type":"permission_prompt"}'
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"PostToolUse","tool_name":"Bash"}'
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"PreToolUse","tool_name":"AskUserQuestion"}'
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"Stop"}'
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"Notification","notification_type":"idle_prompt"}'  # no change
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"SomethingNew"}'                                # no change
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"StopFailure","error":"rate_limit"}'
Invoke-RestMethod -Method Post -Uri $u -ContentType 'application/json' -Body '{"hook_event_name":"SessionEnd","reason":"other"}'
```

A script that does all of this step by step, tells you what you should see each time, and
includes a ~300 KB large-body test:

```powershell
.\tools\Test-Light.ps1 -Ip $ip
```

### 3d. Enable the hooks
Once the manual tests look right:

```powershell
.\tools\Install-Hooks.ps1 -Ip $ip
```

The script:
- Checks that the ESP32 is reachable at `http://<ip>/status` first.
- **Backs up** `~/.claude/settings.json` (`settings.json.bak-<timestamp>`).
- Adds only this project's entries, **keeping your existing settings and any other hooks**.
- Replaces old entries if you run it again, so just rerun it when the IP changes.
- `-WhatIf` shows the result without writing. `.\tools\Install-Hooks.ps1 -Uninstall` removes them.
- Installs `command` hooks that run `curl.exe` and send only the event name.
  Older `type: "http"` entries from earlier versions are replaced automatically.
- `-Timeout 3` gives the ESP32 more time to answer (default 2 s).

Claude Code watches the settings file, so running sessions pick up the change. Then ask Claude
Code something: the light should turn red, then green when the answer is done.

> **If the script won't run** ("running scripts is disabled" in Windows PowerShell):
> `powershell -ExecutionPolicy Bypass -File .\tools\Install-Hooks.ps1 -Ip $ip`

<details>
<summary>The block that gets added, if you'd rather merge it by hand</summary>

It goes under a top-level `"hooks"` key in `~/.claude/settings.json`. If the file already has
`"hooks"`, append each group to the matching event's array and don't delete existing entries.

Every entry has the same shape; only the event name in the URL changes:

```json
"hooks": {
  "UserPromptSubmit": [
    { "hooks": [ { "type": "command", "timeout": 5,
      "command": "curl.exe -s --connect-timeout 2 -m 3 -X POST \"http://192.168.1.50/claude-hook?event=UserPromptSubmit\"; exit 0" } ] }
  ],
  "PostToolUse":        [ "...same, with ?event=PostToolUse..." ],
  "PostToolUseFailure": [ "...same, with ?event=PostToolUseFailure..." ],
  "Notification": [
    { "matcher": "permission_prompt|elicitation_dialog",
      "hooks": [ { "type": "command", "timeout": 5,
      "command": "curl.exe -s --connect-timeout 2 -m 3 -X POST \"http://192.168.1.50/claude-hook?event=Notification\"; exit 0" } ] }
  ],
  "PreToolUse": [
    { "matcher": "AskUserQuestion|ExitPlanMode",
      "hooks": [ { "type": "command", "timeout": 5,
      "command": "curl.exe -s --connect-timeout 2 -m 3 -X POST \"http://192.168.1.50/claude-hook?event=PreToolUse\"; exit 0" } ] }
  ],
  "Stop":        [ "...same, with ?event=Stop..." ],
  "StopFailure": [ "...same, with ?event=StopFailure..." ],
  "SessionEnd":  [ "...same, with ?event=SessionEnd..." ]
}
```
- The command runs in Git Bash, or in PowerShell when Git Bash isn't installed; it works in both.
- `; exit 0` keeps Claude Code from showing a "hook error" on every tool call while the ESP32 is off.
- Events without a `matcher` always fire.
- `PreToolUse` is limited to the two tools that ask you something. The firmware also ignores
  `PreToolUse` from any other tool.
- `idle_prompt` is left out on purpose: it fires a while after a task ends and would turn green
  into yellow. The firmware ignores it as a second safety net.
- `timeout` is Claude Code's own limit in seconds, a safety net above curl's.
  Field and event names were checked against the
  [Claude Code hooks documentation](https://code.claude.com/docs/en/hooks) (November 2026).
</details>

### What goes over the network

Claude Code gives every hook a JSON payload on stdin: your prompt (`UserPromptSubmit`), the full
tool input and output (`PostToolUse`, e.g. a whole file that was read), Claude's last message
(`Stop`), and so on. A `type: "http"` hook would POST all of that, unencrypted, to the ESP32.

The installed hooks are `command` hooks instead. Their command never reads stdin; it only sends a
fixed URL such as `http://<ip>/claude-hook?event=Stop`. So the only things on the wire are the
event name and the ESP32's empty `200` reply. There's nothing worth encrypting, and a device that
somehow ends up with the ESP32's IP learns only *when* Claude Code was busy.

Still, give the ESP32 a fixed IP with a DHCP reservation (see above) so the address can't move to
another device.

---

## 4. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| One LED never lights | LED is in backwards (long leg to the resistor/pin, short leg to GND). Check each one with `/state?name=test`. |
| Too bright overall | Use the brightness slider on the status page. |
| Green is very bright, red is dim | Normal, LEDs differ in efficiency. Lower `MAX_LEVEL_GREEN` in `config.h` to balance the colors. |
| Upload fails (`Failed to connect` / `Write timeout`) | Close any open serial monitor (it holds the COM port). Enter download mode by hand: hold BOOT, tap EN, release BOOT, then upload. Make sure the USB cable carries data. |
| Upload starts but the board drops out of download mode | esptool is resetting it. Make sure `board_upload.before_reset = no_reset` is in `platformio.ini`. |
| Keeps rebooting (brownout) | Weak USB port or cable. Try another port or cable. |
| WiFi animation never ends | Wrong SSID or password, or a 5 GHz network. Check the `[wifi]` lines on the serial monitor. On WPA3-only networks switch the router to WPA2/WPA3 mixed mode. |
| Status page won't open | Are the PC and ESP32 on the same network? Guest networks and "AP/client isolation" stop devices from seeing each other. An active VPN can also block LAN access. |
| Manual tests work, Claude Code doesn't change the light | Is `hook_count` going up on `http://<ip>/status`? If not: is the IP in `~/.claude/settings.json` correct? Start `claude --debug` and look for `Hooks:` lines. If an organization admin set `allowedHttpHookUrls`, this URL must be on that list. |
| Light stays red after pressing Esc | Claude Code doesn't send `Stop` when you interrupt it. After 20 minutes with no events the light turns dim red, and your next message fixes it anyway. The timeout is `WORKING_STALE_MS` in `config.h`. Every tool call resets it; only a single command running longer than 20 minutes with no events in between triggers it early. |
| Stays yellow after denying a permission | It can stay yellow until the next event arrives. |
| The first update after a long idle sometimes gets missed | The first request after the network has been idle can take 1-2 s. If you see this, give curl more time: `.\tools\Install-Hooks.ps1 -Ip $ip -Timeout 3`. |
| Claude Code feels slow while the ESP32 is off | Each hook's curl waits about 2 s for an unreachable ESP32, and `PostToolUse` fires on every tool call. If the light will be off for a while, run `.\tools\Install-Hooks.ps1 -Uninstall`. |
| OTA: "Password rejected" | The board runs firmware built with a different `OTA_PASSWORD`. Flash once over USB. |
| Several Claude Code windows open | For now the most recent event wins (see "Next steps"). |

Every request shows up on the serial monitor:
```
[hook] PostToolUse (Bash) -> working
[hook] Notification (permission_prompt) -> waiting
[hook] PreToolUse (AskUserQuestion) -> waiting
[hook] Notification (idle_prompt) -> (no change)
[state] working -> done
```

---

## 5. How it works (briefly)

- The **web server** (`ESPAsyncWebServer`) runs in its own FreeRTOS task. LED effects advance
  in `loop()` based on `millis()`. Neither ever waits on the other; there's no `delay()`.
- A request only writes the requested state into an atomic variable. The state machine and the
  LEDs change only inside `loop()`, so there are no cross-core races.
- **Two request forms:** the installed hooks send `?event=<name>` (plus optional `&type=` /
  `&tool=`), so the ESP32 normally receives no body at all. It also accepts a full Claude Code JSON
  body, for manual tests and for `type: "http"` hooks if you prefer those.
- **Large JSON bodies:** a `PostToolUse` body contains the tool's whole output and can be hundreds
  of KB. It is never buffered. `include/hook_scanner.h` reads it chunk by chunk and extracts only
  the top-level `hook_event_name`, `notification_type`, `tool_name` and `session_id` fields using
  ~230 bytes of RAM. A fake `"hook_event_name"` inside a tool's output can't be mistaken for the
  real top-level field.
- If the same state arrives again (e.g. back-to-back `PostToolUse`), the effect doesn't restart,
  so the breathing stays smooth.
- PWM: LEDC, 4 kHz, 14 bits, brightness gamma-corrected with 2.2. 14 bits keeps the breathing
  effect smooth even with the overall brightness turned down low.
  The code builds with both the Arduino core 2.x and 3.x LEDC APIs.

### Running the scanner test on a PC
```powershell
g++ -std=c++17 -Wall -I include test/host/test_hook_scanner.cpp -o scanner_test.exe; .\scanner_test.exe
```

### Files
```
platformio.ini              Board, platform, library versions, USB and OTA upload settings
include/config.h            Pins, PWM, effect timings, port, path
include/secrets.example.h   WiFi/OTA template (copy to secrets.h)
include/hook_scanner.h      Streaming JSON field reader
src/main.cpp                State machine, effects, WiFi, HTTP server, OTA
tools/Test-Light.ps1        Step-by-step manual test
tools/Install-Hooks.ps1     Add/remove the curl hooks in settings.json (with backup)
tools/Flash-Ota.ps1         Firmware upload over WiFi (called by pio run -e ota -t upload)
test/host/                  Host-side test for the scanner
```

---

## 6. Next steps (the code is ready for these)

- **Physical approve/deny buttons:** a `PermissionRequest` hook can return a decision:
  `{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}}`
  (or `"deny"`). There's a comment for this in `onHookRequest()`. The async server can hold the
  request and answer it when a button is pressed; the hook command would print the ESP32's reply
  to stdout. That hook needs a timeout longer than a few seconds.
- **Multiple sessions:** the JSON form already parses `session_id` (`HookBodyScanner::sessionId`).
  The curl hooks would need to pass it along too, e.g. as `&session=`, which means reading it from
  stdin. A per-session state table with "any session waiting -> yellow" priority can then be
  added after `mapHookEvent()`.
