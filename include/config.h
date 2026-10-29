#pragma once
#include <stdint.h>
#include "config_secrets.h"

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
// Empty SSID starts a private access point. Otherwise join this 2.4 GHz LAN.
constexpr char WifiSsid[] = "";
constexpr char WifiPassword[] = "";
constexpr char ApSsid[] = "Moonlight-S3";
constexpr char ApPassword[] = "MoonlightS3";
constexpr char Token[] = MOONLIGHT_PAIRING_TOKEN;
static_assert(sizeof(Token) == 33, "Pairing token must be 32 ASCII hex characters");
}
