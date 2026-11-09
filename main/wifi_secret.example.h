#pragma once

/* Optional: home WiFi built into the firmware.
 * Copy this file to wifi_secret.h (it is git-ignored) and fill in your network.
 * It is used only while no WiFi has been saved from the setup page or the
 * console, and never again after a WiFi reset with the BOOT button.
 * Leave WIFI_PASS empty to always start in setup mode instead. */
#define WIFI_SSID "YourNetwork"
#define WIFI_PASS ""
