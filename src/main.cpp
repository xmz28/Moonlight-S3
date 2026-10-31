#include <Arduino.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include "config.h"
#include "display.h"
#include "touch.h"
#include "lan.h"
#include "ui.h"
#include "pairing.h"
#include "storage.h"
#include "performance.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error This firmware requires ESP32-S3
#endif

// Arduino owns the socket, core 1 decodes, core 0 presents completed RGB buffers.
struct Slot { uint8_t *data; uint32_t length, sequence, crc, generation; };
struct Result { uint32_t sequence, decodeUs, totalUs, generation; bool ok; };
struct RenderJob { display::Frame frame; Result result; };
static Slot slots[2];
static QueueHandle_t freeSlots, readySlots, results, renderJobs;
static WiFiServer server(config::Port);
static WiFiClient client;
static uint8_t header[292];
enum class Handshake { Hello, PairProof, AuthProof };
static Handshake handshake=Handshake::Hello;
static size_t have, received;
static int receiving=-1;
static bool authenticated, touchArmed;
static bool paused=false,menuContact=false;
static bool pauseReady=false;
static uint32_t pauseSerial=0,activeOptions=0;
static bool physicalDown=false;
static uint32_t authRequest=0;
static char authHost[33]={};
static uint32_t generation, lastRx, lastTouch, lastTouchSent, lastPrint, shown, errors;
static uint16_t touchX, touchY;
static bool touchDown;
static bool networkNotice=true;

static uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
static void put32(uint8_t *p,uint32_t v) {
  p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
static void disconnect() {
  if (authenticated) {networkNotice=true;lan::streamEnded();ui::streamEnded();}
  client.stop(); authenticated=false; touchArmed=false; have=received=0;
  handshake=Handshake::Hello;pairing::abort();
  paused=pauseReady=false;lan::setLocalPause(false);
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
    RenderJob job={};
    Result &r=job.result;
    r={s.sequence,0,0,s.generation,false};
    if (esp_rom_crc32_le(0,s.data,s.length)==s.crc)
      r.ok=display::prepare(s.data,s.length,job.frame);
    r.decodeUs=job.frame.decodeUs;
    if (!r.ok) Serial.printf("Frame %lu rejected: CRC, JPEG format or LCD synchronization\n",s.sequence);
    if (r.ok) xQueueSend(renderJobs,&job,portMAX_DELAY);
    else {
      display::discard(job.frame);
      xQueueSend(results,&r,portMAX_DELAY);
    }
    // Presenter owns an RGB buffer; compressed bytes can now be reused.
    xQueueSend(freeSlots,&index,portMAX_DELAY);
  }
}
static void renderTask(void *) {
  RenderJob job;
  for (;;) {
    if (xQueueReceive(renderJobs,&job,portMAX_DELAY)!=pdTRUE) continue;
    job.result.ok=display::present(job.frame,job.result.totalUs);
    // ACK only after the real LCD buffer switch has completed.
    xQueueSend(results,&job.result,portMAX_DELAY);
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
  if (!ui::begin()) fail("UI font partition missing or invalid; upload full firmware with font");
  touch::begin();
  freeSlots=xQueueCreate(2,sizeof(int)); readySlots=xQueueCreate(2,sizeof(int));
  results=xQueueCreate(4,sizeof(Result));
  renderJobs=xQueueCreate(2,sizeof(RenderJob));
  if (!freeSlots || !readySlots || !results || !renderJobs) fail("Queue allocation failed");
  for (int i=0;i<2;++i) {
    slots[i].data=static_cast<uint8_t *>(heap_caps_malloc(config::MaxJpeg,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!slots[i].data) fail("JPEG PSRAM allocation failed");
    xQueueSend(freeSlots,&i,0);
  }
  if(!storage::begin())fail("Moonlight configuration migration failed; original NVS preserved");
  if(!performance::begin())Serial.println("Performance overlay unavailable: PSRAM allocation failed");
  lan::begin();
  pairing::begin();
  ui::poll(true);
  if (xTaskCreatePinnedToCore(renderTask,"lcd_present",4096,nullptr,3,nullptr,0)!=pdPASS)
    fail("Presenter task allocation failed");
  if (xTaskCreatePinnedToCore(decodeTask,"jpeg_decode",8192,nullptr,3,nullptr,1)!=pdPASS)
    fail("Decoder task allocation failed");
}
void loop() {
  pairing::poll();
  if (lan::poll()) {
    disconnect(); server.end(); ++generation; networkNotice=true;
    if (lan::connected()) { server.begin(); server.setNoDelay(true); }
  }
  Result r;
  while (xQueueReceive(results,&r,0)==pdTRUE) {
    if(authenticated&&r.generation==generation)performance::displayed(r.decodeUs,r.totalUs,r.ok);
    if (r.ok) ++shown; else ++errors;
    if(r.ok&&authenticated&&r.generation==generation)lan::frameShown();
    if (authenticated && r.generation==generation)
      sendEvent(r.ok?"ACK1":"ERR1",r.sequence,r.decodeUs,r.totalUs);
  }
  // Only update the LCD when all compressed slots are free: decoder cannot race splash.
  if (networkNotice && !authenticated && uxQueueMessagesWaiting(freeSlots)==2 && display::idle()) {
    ui::invalidate(); networkNotice=false;
  }
  const bool uiAllowed=(!authenticated||(paused&&pauseReady))&&uxQueueMessagesWaiting(freeSlots)==2&&display::idle()&&uxQueueMessagesWaiting(results)==0;
  ui::poll(uiAllowed);
  if (client && ui::active()&&!paused) disconnect();
  if (!ui::active() && lan::connected() && (!client || !client.connected())) {
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
    const size_t need=authenticated?16:handshake==Handshake::PairProof?292:handshake==Handshake::AuthProof?68:have<4?4:!memcmp(header,"AU3?",4)?40:36;
    if (receiving<0 && have<need && client.available()) {
      const int n=client.read(header+have,min<size_t>(need-have,client.available()));
      if (n>0) { have+=n; lastRx=millis(); }
    }
    if (!authenticated && have==need && have>=36) {
      if (handshake==Handshake::Hello) {
        if (!memcmp(header,"PAR2",4)) {
          uint8_t reply[288];
          if (!pairing::start(header+4,reply) || client.write(reply,sizeof(reply))!=sizeof(reply)) disconnect();
          else {handshake=Handshake::PairProof;have=0;lastRx=millis();}
        } else if (!memcmp(header,"AU3?",4)) {
          uint8_t reply[48];
          memcpy(authHost,header+4,32);authHost[32]=0;authRequest=be32(header+36);
          if (!lan::authorizeRequest(authHost,authRequest)||!pairing::challenge(header+4,reply) || client.write(reply,sizeof(reply))!=sizeof(reply)) disconnect();
          else {handshake=Handshake::AuthProof;have=0;lastRx=millis();}
        } else disconnect(); // AUT1 and the compiled shared key are no longer accepted.
      } else if (handshake==Handshake::PairProof) {
        uint8_t reply[48];
        if (pairing::finish(header,reply)) client.write(reply,sizeof(reply));
        ui::invalidate();disconnect();
      } else {
        uint8_t reply[68];
        if (!lan::authorizeRequest(authHost,authRequest)||!pairing::authenticate(header,reply) || client.write(reply,sizeof(reply))!=sizeof(reply)) disconnect();
        else {
          authenticated=true;activeOptions=lan::options();have=0;touchArmed=false;lan::streamStarted();performance::reset();performance::enable((lan::options()&2)!=0);
          sendEvent("RDY1",800,480,config::MaxJpeg);
        }
      }
    } else if (authenticated && receiving<0 && have==16) {
      const uint32_t length=be32(header+8);
      if(!memcmp(header,"PAK3",4)){if(paused&&be32(header+4)==pauseSerial)pauseReady=true;have=0;}
      else if(!memcmp(header,"STA3",4)){performance::enable(be32(header+8)==1);performance::host(be32(header+4),WiFi.RSSI());have=0;}
      else if(!memcmp(header,"NOP3",4)){have=0;}
      else if (memcmp(header,"JPG1",4) || length<4 || length>config::MaxJpeg) disconnect();
      else if (xQueueReceive(freeSlots,&receiving,0)==pdTRUE) {
        Slot &s=slots[receiving];
        s.sequence=be32(header+4); s.length=length; s.crc=be32(header+12);
        s.generation=generation; received=0;
      }
    }
    if (receiving>=0 && client.available()) {
      Slot &s=slots[receiving];
      const int n=client.read(s.data+received,min<size_t>(4096,min<size_t>(s.length-received,client.available())));
      if (n>0) { received+=n;performance::received(n); lastRx=millis(); }
      if (received==s.length) {
        if(paused){sendEvent("DRP3",s.sequence,0,0);if(receiving>=0)xQueueSend(freeSlots,&receiving,0);}
        else xQueueSend(readySlots,&receiving,0);
        receiving=-1; have=0;
      }
    }
  }
  if (millis()-lastTouch>=10) {
    lastTouch=millis(); const auto t=touch::poll();
    const bool firstContact=t.down&&!physicalDown;physicalDown=t.down;
    if(!t.down)ui::touch(t); // Always rearm on lift, including while a UI redraw is busy.
    else if (((!authenticated&&!client)||(paused&&pauseReady)) && uxQueueMessagesWaiting(freeSlots)==2 && display::idle()) ui::touch(t);
    if(!t.down)menuContact=false;
    const bool toolbar=(lan::options()&4)!=0;
    if(authenticated&&!paused&&firstContact&&!menuContact&&t.y>=448&&((t.x>=666)||(toolbar&&t.x>=400&&t.x<533))){
      menuContact=true;paused=true;pauseReady=false;++pauseSerial;lan::setLocalPause(true);touchArmed=false;
      if(t.x<533)ui::openText();else ui::openMenu();sendEvent("PAU3",1,pauseSerial,0);
    } else if(authenticated&&!paused&&firstContact&&toolbar&&t.y>=448&&t.x>=533&&t.x<666){
      menuContact=true;sendEvent("BYE3",8,0,0);lan::cancelPlay();disconnect();ui::streamEnded();
    }
    if (!t.down) touchArmed=true;
    const bool down=touchArmed && t.down&&!paused&&!menuContact;
    const bool changed=t.x!=touchX || t.y!=touchY || down!=touchDown;
    // Changes retain the 10 ms polling response. Idle state needs only a
    // 50 ms heartbeat, well inside the host's 350 ms release watchdog.
    if (authenticated && (changed || millis()-lastTouchSent>=(down?20U:50U))) {
      lastTouchSent=millis(); touchX=t.x; touchY=t.y; touchDown=down;
      sendEvent("TCH1",t.x,t.y,down?1:0);
    }
  }
  const auto action=ui::takeAction();
  if(action==ui::Action::Resume&&authenticated){
    const uint32_t nextOptions=lan::options(),changed=nextOptions^activeOptions;
    if(changed&~3UL){
      sendEvent("BYE3",0,0,0);lan::cancelPlay();disconnect();ui::connecting(lan::requestPlay(lan::selectedId()));
    }else{
      if(changed&1)sendEvent("MOD3",nextOptions&1,0,0);
      if(authenticated&&(changed&2)){performance::enable((nextOptions&2)!=0);sendEvent("OVR3",(nextOptions&2)?1:0,0,0);}
      if(authenticated){activeOptions=nextOptions;ui::invalidate();paused=pauseReady=false;lan::setLocalPause(false);touchArmed=false;performance::reset();sendEvent("PAU3",0,pauseSerial,0);}
    }
  }
  else if(action==ui::Action::Disconnect){if(authenticated)sendEvent("BYE3",8,0,0);lan::cancelPlay();disconnect();ui::streamEnded();}
  else if(action==ui::Action::Reconnect){if(authenticated)sendEvent("BYE3",0,0,0);lan::cancelPlay();disconnect();ui::connecting(lan::requestPlay(lan::selectedId()));}
  else if(action==ui::Action::Key&&authenticated&&paused)sendEvent("KEY3",ui::actionValue(),0,0);
  else if(action==ui::Action::Mode&&authenticated&&paused){activeOptions=(activeOptions&~1UL)|(lan::options()&1);sendEvent("MOD3",ui::actionValue(),0,0);}
  else if(action==ui::Action::Stats&&authenticated&&paused){activeOptions=(activeOptions&~2UL)|(lan::options()&2);performance::enable((lan::options()&2)!=0);sendEvent("OVR3",(lan::options()&2)?1:0,0,0);}
  else if(action==ui::Action::Text&&authenticated&&paused){const char *value=ui::textValue();const size_t length=strlen(value);if(length&&length<=128&&sendEvent("TXT3",length,0,0))if(client.write((const uint8_t *)value,length)!=length)disconnect();}
  if (millis()-lastPrint>=2000) {
    Serial.printf("display=%.1f fps errors=%lu internal=%u psram=%u\n",
      shown*1000.0f/(millis()-lastPrint),errors,ESP.getFreeHeap(),ESP.getFreePsram());
    shown=0; errors=0; lastPrint=millis();
  }
  delay(1);
}
