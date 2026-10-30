#pragma once
#include <stdint.h>
namespace performance {
bool begin();
void enable(bool value);
void reset();
void received(uint32_t bytes);
void displayed(uint32_t decodeUs,uint32_t totalUs,bool ok);
void host(uint32_t ackUs,int rssi);
bool beginFrame();
void paintRow(uint16_t *internalRow,int logicalY);
void endFrame();
void benchmark(); // Boot-idle USB diagnostics; never touches the displayed buffer.
}
