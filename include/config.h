#pragma once
#include <stdint.h>
#if __has_include("config_secrets.h")
#include "config_secrets.h"
#endif
#ifndef MOONLIGHT_WIFI_SSID
#define MOONLIGHT_WIFI_SSID ""
#endif
#ifndef MOONLIGHT_WIFI_PASSWORD
#define MOONLIGHT_WIFI_PASSWORD ""
#endif

// Wiring and panel timing copied from LVGL-S3, ESP32-S3-N16R8.
namespace config {
constexpr int Width = 800, Height = 480;
constexpr int DataPins[16] = {4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21};
constexpr int Pclk = 1, Hsync = 39, Vsync = 40, De = 41, Backlight = 42;
constexpr int TouchSda = 47, TouchScl = 48, TouchInt = 2;
constexpr bool TouchSwapXY = false, TouchInvertX = false, TouchInvertY = false;
constexpr uint32_t PixelClock = 12000000; // ~25.36 scans/s, same as LVGL-S3 WiFi mode
constexpr uint16_t Port = 47989;
constexpr uint32_t MaxJpeg = 256 * 1024;
// STA only. USB provisioned NVS credentials take precedence over these defaults.
constexpr char WifiSsid[] = MOONLIGHT_WIFI_SSID;
constexpr char WifiPassword[] = MOONLIGHT_WIFI_PASSWORD;
constexpr uint16_t DiscoveryPort = 47990;
// Pairing credentials are generated during physical PIN authorization and saved in NVS.
}
