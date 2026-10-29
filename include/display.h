#pragma once
#include <stddef.h>
#include <stdint.h>
namespace display {
bool begin();
void splash(const char *network, const char *ip);
// Returns only after the previous framebuffer is safe to reuse.
bool present(const uint8_t *jpeg, size_t size, uint32_t &decodeUs, uint32_t &totalUs);
}
