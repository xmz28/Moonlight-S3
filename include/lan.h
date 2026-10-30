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
enum class PlayState { Idle, Waiting, Accepted, Pin, Verifying, Preparing, FirstFrame, Streaming, Declined, Failed, Disconnected, Cancelled, TimedOut };
PlayState playState();
struct Computer { char id[33],name[49],ip[16];bool paired=false,busy=false,online=false,saved=false;uint8_t monitors=1;uint32_t seen=0; };
void scanComputers();
int computerCount();
bool computer(int index,Computer &out);
uint32_t computerRevision();
bool requestPlay(const char *computerId);
void cancelPlay();
const char *selectedName();
bool needsPin();
void streamStarted();
void frameShown();
void streamEnded(uint32_t reason=1);
bool authorizeRequest(const char *host,uint32_t request);
uint32_t requestId();
const char *message();
uint32_t options(const char *host=nullptr);
bool saveOptions(const char *host,uint32_t value);
const char *selectedId();
void setLocalPause(bool paused);
}
