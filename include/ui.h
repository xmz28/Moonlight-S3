#pragma once
#include "touch.h"
namespace ui {
bool begin();
bool active();
void invalidate();
void poll(bool allowed);
void touch(const touch::State &state);
}
