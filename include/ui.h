#pragma once
#include "touch.h"
namespace ui {
bool begin();
bool active();
void invalidate();
void streamEnded();
void connecting(bool accepted);
void poll(bool allowed);
void touch(const touch::State &state);
enum class Action { None, Resume, Disconnect, Reconnect, Text, Key, Mode };
void openMenu();
void openText();
bool localMenu();
Action takeAction();
const char *textValue();
uint32_t actionValue();
}
