#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "touch.h"

namespace touch {
static uint8_t address;
static State state;
static uint32_t lastValid;
static bool readReg(uint16_t reg, uint8_t *data, size_t size) {
  Wire.beginTransmission(address);
  Wire.write(reg >> 8); Wire.write(reg & 255);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, size) != size) return false;
  for (size_t i=0; i<size; ++i) data[i]=Wire.read();
  return true;
}
void begin() {
  pinMode(config::TouchInt, INPUT_PULLUP);
  if (!Wire.begin(config::TouchSda, config::TouchScl, 400000)) return;
  Wire.setTimeOut(10);
  for (uint8_t candidate : {0x5D, 0x14}) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0) { address=candidate; break; }
  }
  Serial.printf("GT911 address=0x%02X (0 means unavailable)\n", address);
}
State poll() {
  uint8_t status=0, point[4];
  if (address && readReg(0x814E, &status, 1) && (status & 0x80)) {
    const int count=status & 15;
    bool valid=count == 0;
    if (count == 0) state.down=false;
    else if (count<=5 && readReg(0x8150, point, sizeof(point))) {
      int x=point[0] | (point[1]<<8), y=point[2] | (point[3]<<8);
      if (config::TouchSwapXY) { int temp=x; x=y; y=temp; }
      if (config::TouchInvertX) x=config::Width-1-x;
      if (config::TouchInvertY) y=config::Height-1-y;
      state.x=constrain(x, 0, config::Width-1);
      state.y=constrain(y, 0, config::Height-1);
      state.down=true; valid=true;
    }
    Wire.beginTransmission(address);
    Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
    Wire.endTransmission();
    if (valid) lastValid=millis();
  }
  // A dead touch controller must never leave the PC mouse held down.
  if (millis()-lastValid>250) state.down=false;
  return state;
}
} // namespace touch
