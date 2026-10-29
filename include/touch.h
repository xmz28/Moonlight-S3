#pragma once
#include <stdint.h>
namespace touch {
struct State { uint16_t x=0, y=0; bool down=false; };
void begin();
State poll();
}
