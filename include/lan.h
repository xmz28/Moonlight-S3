#pragma once
#include <Arduino.h>
namespace lan {
void begin();
// Maintains STA, discovery and USB configuration; returns true on IP/state change.
bool poll();
bool connected();
const char *name();
String status();
String ssid();
void setConfiguring(bool enabled);
enum class JoinState { Idle, Connecting, Saved, Failed };
JoinState joinState();
bool join(const char *ssid,const char *password);
enum class PlayState { Idle, Waiting, TimedOut };
PlayState playState();
bool requestPlay();
void streamStarted();
}
