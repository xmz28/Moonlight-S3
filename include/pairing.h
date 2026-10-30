#pragma once
#include <Arduino.h>
namespace pairing {
void begin();
void poll();
void open(const char *host,const char *name);
void cancel();
bool enabled();
const char *pin();
const char *status();
bool paired(const char *host=nullptr);
bool forget(const char *host);
int count();
const char *peerId(int index);
const char *peerName(int index);
// First authorization: SRP-6a / RFC5054 2048-bit group / SHA256.
bool start(const uint8_t *host,uint8_t challenge[288]);
bool finish(const uint8_t request[292],uint8_t reply[48]);
// Subsequent authorization: fresh nonce and mutual HMAC, never sends stored key.
bool challenge(const uint8_t *host,uint8_t reply[48]);
bool authenticate(const uint8_t proof[68],uint8_t reply[68]);
void abort();
void signature(const char *host,const char *data,size_t length,char out[65]);
}
