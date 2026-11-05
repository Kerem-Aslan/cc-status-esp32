// Copy this file to "secrets.h" and fill in your own values.
// secrets.h is listed in .gitignore, so it never gets committed.
#pragma once

#define WIFI_SSID     "YourNetworkName"
#define WIFI_PASSWORD "YourWifiPassword"

// Password for over-the-air (OTA) updates. Use something long and random.
// tools/Flash-Ota.ps1 reads it from here. If this line is missing, OTA is compiled out.
#define OTA_PASSWORD  "put-a-long-random-password-here"

// --- Optional: static IP ---
// The recommended way is a DHCP reservation on your router (see README).
// If you can't do that, uncomment ALL FOUR lines below. The IP you pick must be
// OUTSIDE the router's DHCP pool, or it may clash with another device.
// Use commas: 192,168,1,50
//
// #define STATIC_IP       192,168,1,50
// #define STATIC_GATEWAY  192,168,1,1
// #define STATIC_SUBNET   255,255,255,0
// #define STATIC_DNS      192,168,1,1
