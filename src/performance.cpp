#include "performance.h"
#include "font.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <atomic>

namespace performance {
namespace {
constexpr int Width=232,Height=104;
constexpr uint16_t Background=0x08c4,White=0xef9e,Accent=0x3e78;
bool ready=false;
constexpr int Stride=Width/8;
static uint8_t mask[Height][Stride];
alignas(16) static uint16_t pixels[2][256][8];
std::atomic<bool> enabled{false};
portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
struct Sample {uint32_t start=0,bytes=0,frames=0,decode=0,total=0,errors=0,ack=0,epoch=0;int rssi=0;} sample;
uint32_t cachedEpoch=~0U;
uint32_t frameCost=0;
void text(int y,const char *value){
  for(int x=8;*value&&x+8<=Width-8;++value,x+=8){
    unsigned c=static_cast<unsigned char>(*value);if(c<32||c>126)c='?';
    for(int row=0;row<16;++row)mask[y+row][x/8]=Font[c-32][row];
  }
}
}
bool begin(){for(int palette=0;palette<2;++palette)for(int bits=0;bits<256;++bits)for(int x=0;x<8;++x)
  pixels[palette][bits][x]=(bits&(0x80>>x))?(palette?White:Accent):Background;ready=true;return true;}
void reset(){portENTER_CRITICAL(&lock);const uint32_t epoch=sample.epoch+1;sample=Sample{};sample.start=micros();sample.epoch=epoch;portEXIT_CRITICAL(&lock);}
void enable(bool value){if(enabled.exchange(value&&ready)!=value)reset();}
void received(uint32_t bytes){if(!enabled.load(std::memory_order_relaxed))return;portENTER_CRITICAL(&lock);sample.bytes+=bytes;portEXIT_CRITICAL(&lock);}
void displayed(uint32_t decodeUs,uint32_t totalUs,bool ok){if(!enabled.load(std::memory_order_relaxed))return;
  portENTER_CRITICAL(&lock);if(ok){++sample.frames;sample.decode+=decodeUs;sample.total+=totalUs;}else ++sample.errors;portEXIT_CRITICAL(&lock);}
void host(uint32_t ackUs,int rssi){portENTER_CRITICAL(&lock);sample.ack=ackUs;sample.rssi=rssi;portEXIT_CRITICAL(&lock);}
bool beginFrame(){
  if(!ready||!enabled.load(std::memory_order_relaxed))return false;
  const uint32_t start=micros();Sample s;bool update=false,initial=false;
  portENTER_CRITICAL(&lock);
  if(cachedEpoch!=sample.epoch||start-sample.start>=1000000U){
    s=sample;initial=cachedEpoch!=s.epoch;cachedEpoch=s.epoch;update=true;
    sample.start=start;sample.bytes=sample.frames=sample.decode=sample.total=0;
  }
  portEXIT_CRITICAL(&lock);
  if(update){
    memset(mask,0,sizeof(mask));
    char line[40];const float seconds=max(0.001f,(start-s.start)/1000000.0f);
    if(initial)text(7,"Measuring stream...");
    else{snprintf(line,sizeof(line),"%.1f FPS | RX %.2f Mbps",s.frames/seconds,s.bytes*8.0f/seconds/1000000.0f);text(7,line);}
    snprintf(line,sizeof(line),"Decode  %.1f ms",s.frames?s.decode/1000.0f/s.frames:0.0f);text(25,line);
    snprintf(line,sizeof(line),"Display %.1f ms",s.frames?s.total/1000.0f/s.frames:0.0f);text(43,line);
    if(s.ack)snprintf(line,sizeof(line),"ACK %lu ms | Errors %lu",s.ack/1000,s.errors);else snprintf(line,sizeof(line),"ACK -- ms | Errors %lu",s.errors);text(61,line);
    if(s.rssi)snprintf(line,sizeof(line),"Wi-Fi %d dBm",s.rssi);else strlcpy(line,"Wi-Fi -- dBm",sizeof(line));text(79,line);
  }
  frameCost=micros()-start;return true;
}
void paintRow(uint16_t *row,int logicalY){
  const int y=logicalY-16;if(y<0||y>=Height)return;
  const uint32_t start=micros();const int palette=y<24?0:1;
  for(int x=0;x<Stride;++x)memcpy(row+16+x*8,pixels[palette][mask[y][x]],16);
  frameCost+=micros()-start;
}
void endFrame(){
  static uint32_t count=0,elapsed=0;elapsed+=frameCost;
  if(++count==64){Serial.printf("overlay64 avg=%lu us\n",elapsed/count);count=elapsed=0;}
}
void benchmark(){
  uint16_t *scratch=static_cast<uint16_t *>(heap_caps_malloc(800*2,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  if(!scratch||!ready){if(scratch)free(scratch);Serial.println("MS3PERF1 ERROR MEMORY");return;}
  memset(scratch,0,800*2);const bool previous=enabled.load();enable(true);reset();
  const auto frame=[scratch](){beginFrame();for(int y=0;y<480;++y)paintRow(scratch,y);endFrame();};
  uint32_t start=micros();frame();const uint32_t initial=micros()-start;
  uint32_t sum=0,peak=0;
  for(int i=0;i<128;++i){start=micros();frame();const uint32_t cost=micros()-start;sum+=cost;peak=max(peak,cost);delay(1);}
  free(scratch);enable(previous);reset();
  Serial.printf("MS3PERF1 first_us=%lu cached_avg_us=%lu cached_max_us=%lu extra_psram_bytes=0\n",initial,sum/128,peak);
}
}
