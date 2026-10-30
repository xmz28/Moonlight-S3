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
struct Computer { char id[33],name[49],ip[16];bool paired=false;uint32_t seen=0; };
void scanComputers();
int computerCount();
bool computer(int index,Computer &out);
uint32_t computerRevision();
bool requestPlay(const char *computerId);
void cancelPlay();
const char *selectedName();
bool needsPin();
void streamStarted();
}
