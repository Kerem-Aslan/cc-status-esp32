// Hardware and effect settings. To change pins or timings, edit only this file.
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Pins (ESP32-WROOM-32 DevKit V1, right-hand header)
// D25/D26/D27 are not strapping pins and don't affect the boot mode.
// LED anode (long leg) -> resistor -> pin, cathode (short leg) -> GND.
// ---------------------------------------------------------------------------
constexpr uint8_t PIN_RED    = 27;
constexpr uint8_t PIN_YELLOW = 26;
constexpr uint8_t PIN_GREEN  = 25;

// ---------------------------------------------------------------------------
// PWM (LEDC)
// 14 bits at 4 kHz: no visible flicker, and fine steps even when the light is turned
// down low (12 bits made the bottom of the breathing effect look steppy).
// The LEDC clock limits this: frequency * 2^bits must stay under 80 MHz.
// ---------------------------------------------------------------------------
constexpr uint32_t PWM_FREQ_HZ  = 4000;
constexpr uint8_t  PWM_RES_BITS = 14;

// ---------------------------------------------------------------------------
// Overall brightness (percent, perceived). Adjustable live from the status page or
// GET /brightness?value=N, and saved to flash so it survives power cycles.
// ---------------------------------------------------------------------------
constexpr uint8_t  DEFAULT_BRIGHTNESS_PCT   = 50;    // Used until you change it the first time
constexpr uint8_t  MIN_BRIGHTNESS_PCT       = 1;
constexpr uint32_t BRIGHTNESS_SAVE_DELAY_MS = 2000;  // Save once the slider has been still this long

// The eye perceives brightness non-linearly. Without gamma correction most of a fade
// looks "fully on" and the dim end goes by in a flash.
constexpr float GAMMA = 2.2f;

// Maximum brightness per LED (0.0-1.0, perceived). Green LEDs are usually much
// brighter than red ones. Turn it down here if it's too bright on your desk.
constexpr float MAX_LEVEL_RED    = 1.0f;
constexpr float MAX_LEVEL_YELLOW = 1.0f;
constexpr float MAX_LEVEL_GREEN  = 1.0f;

// ---------------------------------------------------------------------------
// Effect timings (ms)
// ---------------------------------------------------------------------------
constexpr uint32_t BREATH_PERIOD_MS = 3000;   // Working: red breathing, one full cycle
constexpr float    BREATH_MIN_LEVEL = 0.03f;  // Bottom of the breath (never fully off)

// Interrupting with Esc doesn't send Stop, so the light would stay red.
// If the light is red and no new event arrives for this long, it switches to a dim
// solid red ("probably finished/interrupted, not sure"). Every tool call resets the timer.
// A single command that runs longer than this without any events will also trigger it.
constexpr uint32_t WORKING_STALE_MS = 20UL * 60UL * 1000UL;  // 20 minutes
constexpr float    STALE_LEVEL      = 0.12f;                 // Dim red brightness

constexpr uint32_t WAIT_ON_MS  = 450;         // Waiting: yellow blinking
constexpr uint32_t WAIT_OFF_MS = 450;

constexpr uint32_t ERROR_ON_MS  = 110;        // Error: all three blink fast
constexpr uint32_t ERROR_OFF_MS = 110;

constexpr uint32_t CONNECT_SWEEP_MS = 1400;   // Waiting for WiFi: light sweeps R->Y->G->Y->R
constexpr float    CONNECT_LEVEL    = 0.6f;

constexpr uint32_t FRAME_INTERVAL_MS = 10;    // LED update interval (~100 fps)

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
constexpr char     HOSTNAME[]  = "cc-status-light";  // Name shown in the router's device list
constexpr uint16_t HTTP_PORT   = 80;
constexpr char     HOOK_PATH[] = "/claude-hook";     // Claude Code hooks POST here

constexpr uint32_t WIFI_RETRY_MS       = 15000;  // Retry interval while disconnected
constexpr uint32_t IP_REMINDER_MS      = 30000;  // Reprint the IP until the first request arrives
