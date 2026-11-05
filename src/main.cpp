// Claude Code Status Light - ESP32 firmware
//
// Claude Code hooks POST to /claude-hook on every state change, either as
// /claude-hook?event=<name> (what tools/Install-Hooks.ps1 sets up: only the event name is sent)
// or with the hook's JSON body (manual tests / type:"http" hooks). The LEDs follow the event:
//
//   UserPromptSubmit, PostToolUse, PostToolUseFailure -> Working : red breathing
//   Notification (permission_prompt / elicitation)    -> Waiting : yellow blinking
//   PreToolUse (AskUserQuestion / ExitPlanMode)       -> Waiting : Claude is asking you something
//   Stop                                              -> Done    : solid green
//   StopFailure                                       -> Error   : all three blink fast
//   SessionEnd                                        -> Off     : all off
//
// Architecture:
//   * The web server (ESPAsyncWebServer) runs in its own task (async_tcp). Requests never
//     wait for LED effects and effects never wait for requests.
//   * The request side only writes the "requested state" into an atomic variable. The state
//     machine and the LEDs change only inside loop(), so there are no cross-core races.
//   * All effects are millis()-based. No delay().

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_arduino_version.h>
#include <atomic>
#include <math.h>

#include "config.h"
#include "hook_scanner.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h not found. Copy include/secrets.example.h to secrets.h and fill in your WiFi credentials."
#endif

#if defined(STATIC_IP) && !(defined(STATIC_GATEWAY) && defined(STATIC_SUBNET) && defined(STATIC_DNS))
#error "secrets.h: if STATIC_IP is enabled, STATIC_GATEWAY, STATIC_SUBNET and STATIC_DNS must be enabled too."
#endif

// ===========================================================================
// States
// ===========================================================================

enum class LightState : uint8_t {
  // Transient (system) states: they end on their own or depend on WiFi.
  BootTest,    // Power-on LED test: R, Y, G one by one, then all together
  Connecting,  // Waiting for WiFi: light sweeps back and forth across the LEDs
  Hello,       // WiFi connected: green flashes twice
  Updating,    // Over-the-air update in progress: yellow/green alternating
  // Claude Code states
  Off,         // No session, or session ended
  Working,     // Agent is working
  Waiting,     // Waiting for approval/input
  Done,        // Finished
  Error,       // Rate limit, API error, etc.
  Stale,       // Red for a long time with no events (probably interrupted with Esc): dim red
  Count
};

// Names for /state?name=... and /status. Order must match the enum.
const char* const STATE_NAMES[] = {
    "test", "connecting", "hello", "updating", "off", "working", "waiting", "done", "error", "stale"};
static_assert(sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]) == (size_t)LightState::Count,
              "STATE_NAMES must have the same length as the enum");

constexpr uint8_t NO_REQUEST = 0xFF;

static inline bool isTransient(LightState s) { return s <= LightState::Updating; }
static inline const char* stateName(LightState s) { return STATE_NAMES[(uint8_t)s]; }

// Hook event -> state mapping. Events not listed here leave the state unchanged.
struct EventMapping {
  const char* event;
  LightState state;
};
const EventMapping EVENT_MAP[] = {
    {"UserPromptSubmit",   LightState::Working},
    {"PostToolUse",        LightState::Working},  // Back to red after you approve a tool
    {"PostToolUseFailure", LightState::Working},  // An approved tool failed; Claude keeps working
    {"Notification",       LightState::Waiting},  // notification_type is checked as well
    {"PreToolUse",         LightState::Waiting},  // Only for tools that ask the user (see below)
    {"Stop",               LightState::Done},
    {"StopFailure",        LightState::Error},
    {"SessionEnd",         LightState::Off},
};

// ===========================================================================
// Shared variables (async_tcp task <-> loop)
// ===========================================================================

AsyncWebServer server(HTTP_PORT);

std::atomic<uint8_t>  g_requested{NO_REQUEST};                     // Written by requests, read by loop
std::atomic<uint8_t>  g_shown{(uint8_t)LightState::BootTest};      // Written by loop, read by web page
std::atomic<uint32_t> g_hookCount{0};
std::atomic<bool>     g_gotAnyRequest{false};
std::atomic<bool>     g_otaActive{false};   // OTA update in progress
std::atomic<uint32_t> g_rebootAtMs{0};      // If non-zero, loop() restarts at this time
std::atomic<uint8_t>  g_brightnessPct{DEFAULT_BRIGHTNESS_PCT};  // Set by /brightness, read by render

// Only read/written from the async_tcp task, no lock needed.
char     g_lastEvent[HookBodyScanner::VAL_MAX] = "-";
uint32_t g_lastEventAtMs = 0;

// loop() side only
LightState state       = LightState::BootTest;
LightState resumeState = LightState::Off;  // State to return to when a transient state ends
uint32_t   stateSinceMs = 0;
uint32_t   lastActivityMs = 0;  // Last hook/manual request (updated even if the state repeats)
bool       wifiUp = false;

// ===========================================================================
// LED driver (LEDC + gamma)
// ===========================================================================

constexpr uint8_t  LED_COUNT = 3;
constexpr uint8_t  LED_PINS[LED_COUNT]  = {PIN_RED, PIN_YELLOW, PIN_GREEN};
constexpr float    LED_MAX[LED_COUNT]   = {MAX_LEVEL_RED, MAX_LEVEL_YELLOW, MAX_LEVEL_GREEN};
constexpr uint32_t DUTY_MAX = (1u << PWM_RES_BITS) - 1;

uint32_t lastDuty[LED_COUNT] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};

// Arduino core 2.x and 3.x have different LEDC APIs. Support both.
void ledInit(uint8_t idx) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LED_PINS[idx], PWM_FREQ_HZ, PWM_RES_BITS);
#else
  ledcSetup(idx, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcAttachPin(LED_PINS[idx], idx);
#endif
}

void ledWriteDuty(uint8_t idx, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LED_PINS[idx], duty);
#else
  ledcWrite(idx, duty);
#endif
}

// level: perceived brightness 0.0-1.0. Gamma is applied here.
void setLed(uint8_t idx, float level) {
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;
  const float perceived = level * LED_MAX[idx];
  const uint32_t duty = (uint32_t)lroundf(powf(perceived, GAMMA) * DUTY_MAX);
  if (duty != lastDuty[idx]) {  // Skip redundant LEDC writes
    lastDuty[idx] = duty;
    ledWriteDuty(idx, duty);
  }
}

// ===========================================================================
// Effects: all computed from t = time since the current state started.
// ===========================================================================

// Boot test steps: bit0=red, bit1=yellow, bit2=green
struct Step {
  uint8_t mask;
  uint16_t ms;
};
constexpr Step BOOT_STEPS[] = {
    {0b001, 250}, {0, 100},  // red
    {0b010, 250}, {0, 100},  // yellow
    {0b100, 250}, {0, 100},  // green
    {0b111, 350}, {0, 150},  // all three
};
uint32_t bootTestTotalMs() {
  uint32_t total = 0;
  for (const Step& s : BOOT_STEPS) total += s.ms;
  return total;
}
const uint32_t BOOT_TEST_MS = bootTestTotalMs();
constexpr uint32_t HELLO_MS     = 650;

uint8_t bootTestMask(uint32_t t) {
  for (const Step& s : BOOT_STEPS) {
    if (t < s.ms) return s.mask;
    t -= s.ms;
  }
  return 0;
}

// Square wave: on for onMs, then off for offMs
static inline float blink(uint32_t t, uint32_t onMs, uint32_t offMs) {
  return (t % (onMs + offMs)) < onMs ? 1.0f : 0.0f;
}

// Breathing: smooth cosine in/out, bottoms out at BREATH_MIN_LEVEL.
// Gamma is applied in setLed(), so a perceptually linear curve is enough here.
static inline float breath(uint32_t t) {
  const float phase = (float)(t % BREATH_PERIOD_MS) / BREATH_PERIOD_MS;
  const float wave = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);  // 0 -> 1 -> 0
  return BREATH_MIN_LEVEL + (1.0f - BREATH_MIN_LEVEL) * wave;
}

void render(uint32_t now) {
  const uint32_t t = now - stateSinceMs;
  float lv[LED_COUNT] = {0, 0, 0};  // red, yellow, green

  switch (state) {
    case LightState::BootTest: {
      const uint8_t m = bootTestMask(t);
      for (uint8_t i = 0; i < LED_COUNT; ++i) lv[i] = (m >> i) & 1 ? 1.0f : 0.0f;
      break;
    }
    case LightState::Connecting: {
      // pos sweeps 0 -> 2 -> 0. Each LED lights up when pos is close to it.
      const float phase = (float)(t % CONNECT_SWEEP_MS) / CONNECT_SWEEP_MS;
      const float pos = phase < 0.5f ? phase * 4.0f : (1.0f - phase) * 4.0f;
      for (uint8_t i = 0; i < LED_COUNT; ++i) {
        const float d = fabsf(pos - i);
        lv[i] = d < 1.0f ? (1.0f - d) * CONNECT_LEVEL : 0.0f;
      }
      break;
    }
    case LightState::Hello:
      lv[2] = blink(t, 150, 150);
      break;
    case LightState::Updating:
      lv[1] = blink(t, 150, 150);
      lv[2] = 1.0f - lv[1];
      break;
    case LightState::Off:
      break;
    case LightState::Working:
      lv[0] = breath(t);
      break;
    case LightState::Waiting:
      lv[1] = blink(t, WAIT_ON_MS, WAIT_OFF_MS);
      break;
    case LightState::Done:
      lv[2] = 1.0f;
      break;
    case LightState::Error:
      lv[0] = lv[1] = lv[2] = blink(t, ERROR_ON_MS, ERROR_OFF_MS);
      break;
    case LightState::Stale:
      lv[0] = STALE_LEVEL;
      break;
    default:
      break;
  }

  // Overall brightness scales the perceived level, before gamma, so 50% looks like half as bright.
  const float brightness = g_brightnessPct.load() / 100.0f;
  for (uint8_t i = 0; i < LED_COUNT; ++i) setLed(i, lv[i] * brightness);
}

// ===========================================================================
// Brightness persistence (NVS flash via Preferences)
// ===========================================================================

Preferences prefs;
uint8_t  savedBrightnessPct = 0;
uint8_t  lastSeenBrightnessPct = 0;
uint32_t brightnessChangedAtMs = 0;

uint8_t clampBrightness(long pct) {
  if (pct < MIN_BRIGHTNESS_PCT) return MIN_BRIGHTNESS_PCT;
  if (pct > 100) return 100;
  return (uint8_t)pct;
}

void loadBrightness() {
  prefs.begin("cclight", false);
  savedBrightnessPct = clampBrightness(prefs.getUChar("bright", DEFAULT_BRIGHTNESS_PCT));
  lastSeenBrightnessPct = savedBrightnessPct;
  g_brightnessPct.store(savedBrightnessPct);
  Serial.printf("[light] brightness %u%%\n", savedBrightnessPct);
}

// Dragging the slider sends many values. Write to flash only once it has settled,
// to spare the flash from wear.
void saveBrightnessIfSettled(uint32_t now) {
  const uint8_t pct = g_brightnessPct.load();
  if (pct != lastSeenBrightnessPct) {
    lastSeenBrightnessPct = pct;
    brightnessChangedAtMs = now;
  }
  if (pct != savedBrightnessPct && now - brightnessChangedAtMs >= BRIGHTNESS_SAVE_DELAY_MS) {
    prefs.putUChar("bright", pct);
    savedBrightnessPct = pct;
    Serial.printf("[light] brightness %u%% saved\n", pct);
  }
}

// ===========================================================================
// State machine (called from loop() only)
// ===========================================================================

void setState(LightState s) {
  if (s == state) return;  // Same state again: don't restart the effect (keeps breathing smooth)
  Serial.printf("[state] %s -> %s\n", stateName(state), stateName(s));
  state = s;
  stateSinceMs = millis();
  if (s == LightState::Working) lastActivityMs = stateSinceMs;  // Stale timer starts on entering red
  g_shown.store((uint8_t)s);
}

// If red with no events for WORKING_STALE_MS, switch to dim red.
void checkStale(uint32_t now) {
  if (state == LightState::Working && now - lastActivityMs >= WORKING_STALE_MS) {
    Serial.printf("[state] no events for %lu min, probably interrupted\n",
                  (unsigned long)(WORKING_STALE_MS / 60000UL));
    setState(LightState::Stale);
  }
}

// Move on when a transient state is over.
void updateTransients(uint32_t now) {
  const uint32_t t = now - stateSinceMs;
  if (state == LightState::BootTest && t >= BOOT_TEST_MS) {
    setState(wifiUp ? LightState::Hello : LightState::Connecting);
  } else if (state == LightState::Hello && t >= HELLO_MS) {
    setState(resumeState);
  } else if (state == LightState::Updating && !g_otaActive.load()) {
    setState(resumeState);  // Update failed, go back to the previous state
  }
}

// Apply a state request coming from the HTTP side.
void handleRequests() {
  const uint8_t r = g_requested.exchange(NO_REQUEST);
  if (r == NO_REQUEST || r >= (uint8_t)LightState::Count) return;
  const LightState want = (LightState)r;
  lastActivityMs = millis();  // Back-to-back PostToolUse events also reset the stale timer

  // The LED test (/state?name=test) and the update animation take over other transient states
  if (want == LightState::BootTest || want == LightState::Updating) {
    if (!isTransient(state)) resumeState = state;
    setState(want);
  } else if (isTransient(state)) {
    resumeState = want;  // Switch to this once the animation ends
  } else {
    setState(want);
  }
}

// ===========================================================================
// WiFi
// ===========================================================================

uint32_t lastWifiAttemptMs = 0;
uint32_t lastIpReminderMs  = 0;

void printNetInfo() {
  const String ip = WiFi.localIP().toString();
  Serial.println();
  Serial.println("==================================================");
  Serial.printf(" WiFi connected: %s  (RSSI %d dBm)\n", WIFI_SSID, WiFi.RSSI());
  Serial.printf(" IP address : %s\n", ip.c_str());
  Serial.printf(" Hook URL   : http://%s%s\n", ip.c_str(), HOOK_PATH);
  Serial.printf(" Status page: http://%s/\n", ip.c_str());
  Serial.printf(" MAC        : %s  (for a DHCP reservation)\n", WiFi.macAddress().c_str());
  Serial.println("==================================================");
}

void startWifi() {
  WiFi.persistent(false);       // Don't keep rewriting credentials to flash
  WiFi.setHostname(HOSTNAME);   // Must be called before mode()
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);         // Modem sleep adds 100+ ms latency to incoming requests
  WiFi.setAutoReconnect(true);
#ifdef STATIC_IP
  WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GATEWAY), IPAddress(STATIC_SUBNET),
              IPAddress(STATIC_DNS));
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
  Serial.printf("[wifi] connecting to '%s'...\n", WIFI_SSID);
}

void handleWifi(uint32_t now) {
  const bool up = WiFi.status() == WL_CONNECTED;

  if (up && !wifiUp) {
    wifiUp = true;
    printNetInfo();
    lastIpReminderMs = now;
    if (state == LightState::Connecting) setState(LightState::Hello);
  } else if (!up && wifiUp) {
    wifiUp = false;
    lastWifiAttemptMs = now;
    Serial.println("[wifi] connection lost, retrying...");
    if (!isTransient(state)) resumeState = state;
    if (state != LightState::BootTest) setState(LightState::Connecting);
  }

  // Auto-reconnect sometimes gets stuck (e.g. after the router reboots).
  if (!up && now - lastWifiAttemptMs >= WIFI_RETRY_MS) {
    lastWifiAttemptMs = now;
    Serial.println("[wifi] retrying connection...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // In case the serial monitor was opened late: repeat the IP until the first request arrives.
  if (up && !g_gotAnyRequest.load() && now - lastIpReminderMs >= IP_REMINDER_MS) {
    lastIpReminderMs = now;
    Serial.printf("[info] IP: %s  (this reminder stops after the first request)\n",
                  WiFi.localIP().toString().c_str());
  }
}

// ===========================================================================
// HTTP server (runs in the async_tcp task)
// ===========================================================================

// Only notification types that actually need the user turn the light yellow.
// The matcher in settings.json already filters these. This is a second safety net in case
// the matcher is ever widened: in particular idle_prompt must not turn green into yellow.
// If notification_type is missing (manual test), go yellow.
bool notificationNeedsUser(const char* type) {
  return type[0] == '\0' || strcmp(type, "permission_prompt") == 0 ||
         strcmp(type, "elicitation_dialog") == 0;
}

// PreToolUse fires before every tool. Only tools that ask the user turn the light yellow:
// AskUserQuestion (Claude asks you a question) and ExitPlanMode (waiting for plan approval).
// Other tools don't change the state. The matcher in settings.json is limited to these two too.
// If the tool name is missing (query-string hooks, where the matcher already filtered), go yellow.
bool toolAsksUser(const char* tool) {
  return tool[0] == '\0' || strcmp(tool, "AskUserQuestion") == 0 ||
         strcmp(tool, "ExitPlanMode") == 0;
}

// Copies a query parameter into a fixed-size scanner field (empty if missing).
void copyParam(AsyncWebServerRequest* req, const char* name, char* dst, size_t size) {
  dst[0] = '\0';
  if (!req->hasParam(name)) return;
  strncpy(dst, req->getParam(name)->value().c_str(), size - 1);
  dst[size - 1] = '\0';
}

uint8_t mapHookEvent(const HookBodyScanner& sc) {
  for (const EventMapping& m : EVENT_MAP) {
    if (strcmp(sc.hookEventName, m.event) != 0) continue;
    if (strcmp(m.event, "Notification") == 0 && !notificationNeedsUser(sc.notificationType)) {
      return NO_REQUEST;
    }
    if (strcmp(m.event, "PreToolUse") == 0 && !toolAsksUser(sc.toolName)) {
      return NO_REQUEST;
    }
    return (uint8_t)m.state;
  }
  return NO_REQUEST;  // Unknown event: leave the state unchanged
}

// The body arrives in chunks. Each request carries its own scanner in _tempObject.
// The library frees _tempObject with free() when the request is done.
void onHookBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
                size_t total) {
  (void)total;
  if (index == 0 && req->_tempObject == nullptr) {
    auto* sc = static_cast<HookBodyScanner*>(malloc(sizeof(HookBodyScanner)));
    if (sc != nullptr) sc->reset();
    req->_tempObject = sc;
  }
  auto* sc = static_cast<HookBodyScanner*>(req->_tempObject);
  if (sc != nullptr) sc->feed(data, len);
}

void onHookRequest(AsyncWebServerRequest* req) {
  g_gotAnyRequest.store(true);
  auto* sc = static_cast<HookBodyScanner*>(req->_tempObject);

  // Privacy-friendly form used by the installed hooks: only the event name travels in the URL,
  //   POST /claude-hook?event=Stop   (optional &type=<notification_type> &tool=<tool_name>)
  // and the hook's JSON (prompts, file contents, command output) never leaves the PC.
  // The JSON-body form below is still supported for manual tests and type:"http" hooks.
  HookBodyScanner fromQuery;
  if (req->hasParam("event")) {
    fromQuery.reset();
    copyParam(req, "event", fromQuery.hookEventName, sizeof(fromQuery.hookEventName));
    copyParam(req, "type", fromQuery.notificationType, sizeof(fromQuery.notificationType));
    copyParam(req, "tool", fromQuery.toolName, sizeof(fromQuery.toolName));
    sc = &fromQuery;
  }

  if (sc != nullptr && sc->hookEventName[0] != '\0') {
    const uint8_t target = mapHookEvent(*sc);
    if (target != NO_REQUEST) g_requested.store(target);

    // Copy only letters, digits and '_' so the /status JSON can't be broken.
    size_t n = 0;
    for (const char* p = sc->hookEventName; *p && n < sizeof(g_lastEvent) - 1; ++p) {
      if (isalnum((unsigned char)*p) || *p == '_') g_lastEvent[n++] = *p;
    }
    g_lastEvent[n] = '\0';
    g_lastEventAtMs = millis();
    g_hookCount.fetch_add(1);

    // Extra detail: notification type for Notification, tool name for tool events
    const char* detail = sc->notificationType[0] ? sc->notificationType : sc->toolName;
    Serial.printf("[hook] %s%s%s%s -> %s\n", sc->hookEventName,
                  detail[0] ? " (" : "", detail, detail[0] ? ")" : "",
                  target == NO_REQUEST ? "(no change)" : STATE_NAMES[target]);
  } else {
    Serial.println("[hook] request without hook_event_name, ignored");
  }

  // IMPORTANT: 200 with an empty body. Claude Code reads this as "success, no decision".
  // A plain-text body would be counted as an error by Claude Code.
  //
  // Future physical approve/deny buttons (PermissionRequest): instead of answering right away,
  // keep req and reply when a button is pressed with JSON like
  // {"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}}
  // (that hook's timeout must be raised as well).
  //
  // Future multi-session support: sc->sessionId is already available here. Keep a per-session
  // state table and apply "any session waiting -> yellow" priority after mapHookEvent.
  req->send(200);
}

// ---------------------------------------------------------------------------
// Over-the-air update (OTA): POST /update, multipart "firmware" file, Basic auth
//   curl.exe -u ota:<OTA_PASSWORD> -F "firmware=@firmware.bin" http://<ip>/update
// Easy way: pio run -e ota -t upload (calls tools/Flash-Ota.ps1)
// ---------------------------------------------------------------------------
#ifdef OTA_PASSWORD
constexpr char OTA_USER[] = "ota";
AsyncWebServerRequest* g_otaOwner = nullptr;  // Only accessed from the async_tcp task

bool otaAuthorized(AsyncWebServerRequest* req) { return req->authenticate(OTA_USER, OTA_PASSWORD); }

void otaAbort(const char* why) {
  Serial.printf("[ota] aborted: %s\n", why);
  if (Update.isRunning()) Update.abort();
  g_otaOwner = nullptr;
  g_otaActive.store(false);
}

void onOtaChunk(AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data,
                size_t len, bool final) {
  if (index == 0) {
    if (!otaAuthorized(req)) {
      Serial.println("[ota] wrong password, rejected");
      return;
    }
    if (g_otaOwner != nullptr) {
      Serial.println("[ota] another update is already running");
      return;
    }
    Serial.printf("[ota] started: %s (~%u bytes)\n", filename.c_str(), (unsigned)req->contentLength());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Update.printError(Serial);
      return;
    }
    g_otaOwner = req;
    g_otaActive.store(true);
    g_requested.store((uint8_t)LightState::Updating);
    // If the upload is cut off (cable/WiFi/Ctrl+C), clean up the half-written update
    req->onDisconnect([]() {
      if (g_otaOwner != nullptr) otaAbort("connection lost");
    });
  }
  if (req != g_otaOwner) return;

  if (len > 0 && Update.write(data, len) != len) {
    Update.printError(Serial);
  }
  if (final) {
    if (Update.end(true)) {
      Serial.printf("[ota] done: %u bytes written\n", (unsigned)(index + len));
    } else {
      Update.printError(Serial);
    }
  }
}

void onOtaDone(AsyncWebServerRequest* req) {
  if (!otaAuthorized(req)) {
    req->requestAuthentication(AsyncAuthType::AUTH_BASIC);
    return;
  }
  const bool mine = (req == g_otaOwner);
  const bool ok = mine && Update.isFinished() && !Update.hasError();
  if (mine) g_otaOwner = nullptr;

  AsyncWebServerResponse* res = req->beginResponse(
      ok ? 200 : 500, "text/plain",
      ok ? "OK: new firmware written, restarting\n"
         : "ERROR: update failed, check the serial monitor\n");
  res->addHeader("Connection", "close");
  req->send(res);

  if (ok) {
    g_rebootAtMs.store(millis() + 750);  // Short wait so the response reaches the PC
  } else if (mine) {
    otaAbort("verification failed");
  }
}
#endif

// Simple status page for testing from a browser
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Code Status Light</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:520px;margin:24px auto;padding:0 16px;background:#111;color:#eee}
 h1{font-size:20px} .lamp{display:flex;gap:14px;margin:18px 0}
 .l{width:42px;height:42px;border-radius:50%;background:#333;transition:background .2s}
 .r.on{background:#e53935;box-shadow:0 0 16px #e53935}
 .y.on{background:#fdd835;box-shadow:0 0 16px #fdd835}
 .g.on{background:#43a047;box-shadow:0 0 16px #43a047}
 table{border-collapse:collapse;width:100%} td{padding:4px 0;border-bottom:1px solid #222}
 td:first-child{color:#999;width:40%} button{margin:4px 4px 0 0;padding:8px 12px;border:0;border-radius:6px;background:#333;color:#eee;cursor:pointer}
 button:hover{background:#444} code{color:#9cf}
 .bright{display:flex;align-items:center;gap:12px;margin:0 0 16px} .bright input{flex:1;accent-color:#fdd835}
 .bright span{min-width:3.5em;text-align:right;font-variant-numeric:tabular-nums}
</style></head><body>
<h1>Claude Code Status Light</h1>
<div class="lamp"><div id="r" class="l r"></div><div id="y" class="l y"></div><div id="g" class="l g"></div></div>
<label class="bright">Brightness <input id="br" type="range" min="1" max="100" value="50"><span id="brv">-</span></label>
<table>
 <tr><td>State</td><td id="state">-</td></tr>
 <tr><td>Last event</td><td id="ev">-</td></tr>
 <tr><td>Hook count</td><td id="cnt">-</td></tr>
 <tr><td>Hook URL</td><td><code id="url">-</code></td></tr>
 <tr><td>WiFi</td><td id="rssi">-</td></tr>
 <tr><td>Uptime</td><td id="up">-</td></tr>
</table>
<p>
 <button onclick="s('working')">working</button><button onclick="s('waiting')">waiting</button>
 <button onclick="s('done')">done</button><button onclick="s('error')">error</button>
 <button onclick="s('off')">off</button><button onclick="s('stale')">stale</button><button onclick="s('test')">LED test</button>
</p>
<script>
const lamps={working:'r',waiting:'y',done:'g',error:'ryg',test:'ryg',connecting:'',hello:'g',updating:'yg',off:'',stale:'r'};
async function s(n){await fetch('/state?name='+n);setTimeout(u,150)}
// Brightness slider: send at most every 80 ms while dragging, always send the final value
let dragging=false,lastSent=0,pending=null;
function sendBr(v){lastSent=Date.now();fetch('/brightness?value='+v)}
br.addEventListener('input',()=>{dragging=true;brv.textContent=br.value+'%';clearTimeout(pending);
 if(Date.now()-lastSent>=80)sendBr(br.value);else pending=setTimeout(()=>sendBr(br.value),80)});
br.addEventListener('change',()=>{clearTimeout(pending);sendBr(br.value);dragging=false});
async function u(){try{const j=await (await fetch('/status')).json();
 if(!dragging){br.value=j.brightness;brv.textContent=j.brightness+'%'}
 state.textContent=j.state;ev.textContent=j.last_event+(j.last_event_age_s>=0?' ('+j.last_event_age_s+' s ago)':'');
 cnt.textContent=j.hook_count;url.textContent=j.hook_url;rssi.textContent=j.rssi+' dBm';up.textContent=j.uptime_s+' s';
 for(const id of 'ryg')document.getElementById(id).classList.toggle('on',(lamps[j.state]||'').includes(id));
}catch(e){state.textContent='no connection'}}
u();setInterval(u,1500);
</script></body></html>)HTML";

void setupServer() {
  // Claude Code hooks
  server.on(HOOK_PATH, HTTP_POST, onHookRequest, nullptr, onHookBody);

  // Status page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html; charset=utf-8", INDEX_HTML);
  });

  // Machine-readable status
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    const uint32_t now = millis();
    const long age = g_hookCount.load() ? (long)((now - g_lastEventAtMs) / 1000) : -1;
    const String ip = WiFi.localIP().toString();
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"last_event\":\"%s\",\"last_event_age_s\":%ld,"
             "\"hook_count\":%lu,\"hook_url\":\"http://%s%s\",\"ip\":\"%s\","
             "\"rssi\":%d,\"uptime_s\":%lu,\"brightness\":%u}",
             STATE_NAMES[g_shown.load()], g_lastEvent, age, (unsigned long)g_hookCount.load(),
             ip.c_str(), HOOK_PATH, ip.c_str(), WiFi.RSSI(), (unsigned long)(now / 1000),
             (unsigned)g_brightnessPct.load());
    req->send(200, "application/json", buf);
  });

  // Overall brightness: GET /brightness?value=1..100 sets it, GET /brightness reads it
  server.on("/brightness", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("value")) {
      const String v = req->getParam("value")->value();
      if (v.length() == 0 || !isdigit((unsigned char)v[0])) {
        req->send(400, "text/plain", "usage: /brightness?value=1..100\n");
        return;
      }
      g_brightnessPct.store(clampBrightness(v.toInt()));
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"brightness\":%u}", (unsigned)g_brightnessPct.load());
    req->send(200, "application/json", buf);
  });

  // Manual state change: /state?name=working|waiting|done|error|off|stale|test
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("name")) {
      String name = req->getParam("name")->value();
      name.toLowerCase();
      for (uint8_t i = 0; i < (uint8_t)LightState::Count; ++i) {
        const LightState s = (LightState)i;
        if (s == LightState::Connecting || s == LightState::Hello || s == LightState::Updating) {
          continue;  // Not selectable by hand
        }
        if (name == STATE_NAMES[i]) {
          g_gotAnyRequest.store(true);
          g_requested.store(i);
          Serial.printf("[manual] -> %s\n", STATE_NAMES[i]);
          req->send(200, "text/plain", String("ok: ") + STATE_NAMES[i] + "\n");
          return;
        }
      }
    }
    req->send(400, "text/plain",
              "usage: /state?name=working|waiting|done|error|off|stale|test\n");
  });

#ifdef OTA_PASSWORD
  server.on("/update", HTTP_POST, onOtaDone, onOtaChunk);
#endif

  server.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "not found\n"); });
  server.begin();
}

// ===========================================================================
// Arduino entry points
// ===========================================================================

uint32_t lastFrameMs = 0;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Claude Code Status Light starting...");

  for (uint8_t i = 0; i < LED_COUNT; ++i) {
    ledInit(i);
    setLed(i, 0.0f);
  }
  loadBrightness();         // Before the first frame, so the boot test uses the saved level
  stateSinceMs = millis();  // The BootTest effect starts now

  startWifi();     // Non-blocking; the connection is tracked in loop()
  setupServer();   // Must start after WiFi.mode() so the TCP/IP stack is ready
}

void loop() {
  const uint32_t now = millis();

  handleWifi(now);
  handleRequests();
  updateTransients(now);
  checkStale(now);
  saveBrightnessIfSettled(now);

  const uint32_t rebootAt = g_rebootAtMs.load();
  if (rebootAt != 0 && (int32_t)(now - rebootAt) >= 0) {
    Serial.println("[ota] restarting...");
    Serial.flush();
    ESP.restart();
  }

  if (now - lastFrameMs >= FRAME_INTERVAL_MS) {
    lastFrameMs = now;
    render(now);
  }

  // Give other tasks on this core one tick. Effects are millis()-based,
  // so this short wait doesn't affect the animations.
  vTaskDelay(1);
}
