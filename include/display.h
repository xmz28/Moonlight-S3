#pragma once
#include <stddef.h>
#include <stdint.h>
namespace display {
bool begin();
uint16_t *beginUi(int &index);
void endUi(int index);
struct Frame {
  int index=-1;
  uint32_t started=0, decodeUs=0, preparedUs=0;
};
// Decoder prepares a free buffer while the presenter waits for LCD completion.
bool prepare(const uint8_t *jpeg, size_t size, Frame &frame);
bool present(const Frame &frame, uint32_t &totalUs);
void discard(const Frame &frame);
bool idle();
}
