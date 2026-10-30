#pragma once
#include <Arduino.h>
namespace lan {
void begin();
// Maintains STA, discovery and USB configuration; returns true on IP/state change.
bool poll();
bool connected();
const char *name();
String status();
}
