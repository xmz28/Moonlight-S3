#include <Arduino.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include "config.h"
#include "display.h"
#include "touch.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error This firmware requires ESP32-S3
#endif

// Only the Arduino loop owns the socket. The decoder owns the LCD.
struct Slot { uint8_t *data; uint32_t length, sequence, crc, generation; };
struct Result { uint32_t sequence, decodeUs, totalUs, generation; bool ok; };
static Slot slots[2];
static QueueHandle_t freeSlots, readySlots, results;
static WiFiServer server(config::Port);
static WiFiClient client;
static uint8_t header[36];
static size_t have, received;
static int receiving=-1;
static bool authenticated, touchArmed;
static uint32_t generation, lastRx, lastTouch, lastPrint, shown, errors;

static uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
static void put32(uint8_t *p,uint32_t v) {
  p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
static void disconnect() {
  client.stop(); authenticated=false; touchArmed=false; have=received=0;
  if (receiving>=0) { xQueueSend(freeSlots,&receiving,0); receiving=-1; }
}
static bool sendEvent(const char *tag,uint32_t a,uint32_t b,uint32_t c) {
  uint8_t packet[16]; memcpy(packet,tag,4);
  put32(packet+4,a); put32(packet+8,b); put32(packet+12,c);
  if (client.write(packet,sizeof(packet))!=sizeof(packet)) { disconnect(); return false; }
  return true;
}
static void decodeTask(void *) {
  int index;
  for (;;) {
    if (xQueueReceive(readySlots,&index,portMAX_DELAY)!=pdTRUE) continue;
    Slot &s=slots[index];
    Result r={s.sequence,0,0,s.generation,false};
    if (esp_rom_crc32_le(0,s.data,s.length)==s.crc)
      r.ok=display::present(s.data,s.length,r.decodeUs,r.totalUs);
    if (!r.ok) Serial.printf("Frame %lu rejected: CRC, JPEG format or LCD synchronization\n",s.sequence);
    // Copy result before releasing the slot; the receiver may immediately reuse it.
    xQueueSend(results,&r,portMAX_DELAY);
    xQueueSend(freeSlots,&index,portMAX_DELAY);
  }
}
static void fail(const char *message) {
  Serial.println(message);
  for (;;) delay(1000);
}
void setup() {
  Serial.begin(115200); delay(300);
  Serial.printf("Moonlight S3: %s, PSRAM %u bytes\n",ESP.getChipModel(),ESP.getPsramSize());
  if (!psramFound() || ESP.getPsramSize()<4*1024*1024) fail("8 MB OPI PSRAM board expected");
  if (!display::begin()) fail("LCD/JPEG initialization failed");
  touch::begin();
  freeSlots=xQueueCreate(2,sizeof(int)); readySlots=xQueueCreate(2,sizeof(int));
  results=xQueueCreate(4,sizeof(Result));
  if (!freeSlots || !readySlots || !results) fail("Queue allocation failed");
  for (int i=0;i<2;++i) {
    slots[i].data=static_cast<uint8_t *>(heap_caps_malloc(config::MaxJpeg,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!slots[i].data) fail("JPEG PSRAM allocation failed");
    xQueueSend(freeSlots,&i,0);
  }
  bool ap=!config::WifiSsid[0];
  if (!ap) {
    WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setAutoReconnect(true);
    WiFi.begin(config::WifiSsid,config::WifiPassword);
    const uint32_t start=millis();
    while (WiFi.status()!=WL_CONNECTED && millis()-start<15000) delay(100);
    if (WiFi.status()!=WL_CONNECTED) { WiFi.disconnect(true); ap=true; }
  }
  if (ap) {
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(config::ApSsid,config::ApPassword,6,false,1)) fail("WiFi AP failed");
  }
  WiFi.setSleep(false);
  const String ip=(ap?WiFi.softAPIP():WiFi.localIP()).toString();
  Serial.printf("%s: %s TCP %u\n",ap?config::ApSsid:config::WifiSsid,ip.c_str(),config::Port);
  display::splash(ap?config::ApSsid:config::WifiSsid,ip.c_str());
  if (xTaskCreatePinnedToCore(decodeTask,"jpeg_decode",8192,nullptr,3,nullptr,1)!=pdPASS)
    fail("Decoder task allocation failed");
  server.begin(); server.setNoDelay(true);
}
void loop() {
  Result r;
  while (xQueueReceive(results,&r,0)==pdTRUE) {
    if (r.ok) ++shown; else ++errors;
    if (authenticated && r.generation==generation)
      sendEvent(r.ok?"ACK1":"ERR1",r.sequence,r.decodeUs,r.totalUs);
  }
  if (!client || !client.connected()) {
    disconnect();
    client=server.accept();
    if (client) {
      client.setNoDelay(true); client.setConnectionTimeout(250);
      ++generation; lastRx=millis();
      Serial.println("PC connected; waiting for pairing");
    }
  }
  if (client && millis()-lastRx>3000) disconnect();
  if (client) {
    // Read bounded chunks without blocking touch polling or ACK delivery.
    const size_t need=authenticated?16:36;
    if (receiving<0 && have<need && client.available()) {
      const int n=client.read(header+have,min<size_t>(need-have,client.available()));
      if (n>0) { have+=n; lastRx=millis(); }
    }
    if (!authenticated && have==36) {
      uint8_t difference=0;
      for (size_t i=0;i<32;++i) difference|=header[4+i]^config::Token[i];
      if (memcmp(header,"AUT1",4) || difference) disconnect();
      else {
        authenticated=true; have=0; touchArmed=false;
        sendEvent("RDY1",800,480,config::MaxJpeg);
      }
    } else if (authenticated && receiving<0 && have==16) {
      const uint32_t length=be32(header+8);
      if (memcmp(header,"JPG1",4) || length<4 || length>config::MaxJpeg) disconnect();
      else if (xQueueReceive(freeSlots,&receiving,0)==pdTRUE) {
        Slot &s=slots[receiving];
        s.sequence=be32(header+4); s.length=length; s.crc=be32(header+12);
        s.generation=generation; received=0;
      }
    }
    if (receiving>=0 && client.available()) {
      Slot &s=slots[receiving];
      const int n=client.read(s.data+received,min<size_t>(4096,min<size_t>(s.length-received,client.available())));
      if (n>0) { received+=n; lastRx=millis(); }
      if (received==s.length) {
        xQueueSend(readySlots,&receiving,0); receiving=-1; have=0;
      }
    }
  }
  if (millis()-lastTouch>=10) {
    lastTouch=millis(); const auto t=touch::poll();
    if (!t.down) touchArmed=true;
    // Full state every 10 ms; no lost mouse-up and a host-side dead-man timer.
    if (authenticated) sendEvent("TCH1",t.x,t.y,touchArmed && t.down?1:0);
  }
  if (millis()-lastPrint>=2000) {
    Serial.printf("display=%.1f fps errors=%lu internal=%u psram=%u\n",
      shown*1000.0f/(millis()-lastPrint),errors,ESP.getFreeHeap(),ESP.getFreePsram());
    shown=0; errors=0; lastPrint=millis();
  }
  delay(1);
}
