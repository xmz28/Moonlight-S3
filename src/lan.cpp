#include <Arduino.h>
#include <WiFi.h>
#include <NetworkUdp.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <atomic>
#include "config.h"
#include "lan.h"
#include "pairing.h"
#include "storage.h"

namespace lan {
namespace {
struct Credentials { char ssid[33]; char password[65]; } credentials={};
Credentials candidate={};
bool configuring=false;
JoinState joining=JoinState::Idle;
uint32_t joinStarted=0;
std::atomic<uint32_t> associations{0};
uint32_t joinAssociation=0;
uint32_t playId=0,playStarted=0;
PlayState playing=PlayState::Idle;
Computer computers[8];int computersCount=0;uint32_t revision=0;
char selectedComputer[33]={},selectedComputerName[49]={};
bool pinRequired=false,localPaused=false;
uint32_t reason=0;
struct Profile {char host[33];uint32_t value;};
struct Profiles {uint32_t version=1;Profile entries[8]={};} profiles;
constexpr uint32_t DefaultOptions=(25UL<<24)|(60UL<<16)|(1UL<<8)|28;
const char *failure="";
NetworkUDP discovery;
char id[13], hostname[32], line[224];
size_t lineSize=0;
bool overflow=false, online=false, mdns=false;
IPAddress address;
uint32_t lastRetry=0, lastReply=0;
int hex(char c) {
  if (c>='0' && c<='9') return c-'0';
  if (c>='a' && c<='f') return c-'a'+10;
  if (c>='A' && c<='F') return c-'A'+10;
  return -1;
}
bool decodeHex(const char *text,char *out,size_t capacity) {
  const size_t size=strlen(text);
  if (size%2 || size/2>=capacity) return false;
  for (size_t i=0;i<size;i+=2) {
    const int a=hex(text[i]),b=hex(text[i+1]);
    if (a<0 || b<0 || (a==0 && b==0)) return false;
    out[i/2]=(a<<4)|b;
  }
  out[size/2]=0;
  return true;
}
void command() {
  if (!strcmp(line,"MS3STATUS1")) {
    Serial.printf("MS3STATUS1 %s %s %s\n",id,WiFi.localIP().toString().c_str(),
      online?"CONNECTED":credentials.ssid[0]?"WAITING":"UNCONFIGURED");
    wifi_ap_record_t ap={};
    const esp_err_t link=esp_wifi_sta_get_ap_info(&ap);
    Serial.printf("MS3LINK1 wl=%d associated=%d channel=%u rssi=%d\n",
      static_cast<int>(WiFi.status()),link==ESP_OK,link==ESP_OK?ap.primary:0,link==ESP_OK?ap.rssi:0);
    return;
  }
  if (strncmp(line,"MS3WIFI1 ",9)) return;
  char *separator=strchr(line+9,' ');
  Credentials next={};
  if (!separator) { Serial.println("MS3WIFI1 ERROR FORMAT"); return; }
  *separator=0;
  const char *password=separator+1;
  bool ok=decodeHex(line+9,next.ssid,sizeof(next.ssid)) && next.ssid[0];
  if (strcmp(password,"-")) ok=ok && decodeHex(password,next.password,sizeof(next.password));
  const size_t len=strlen(next.password);
  // Empty permits open LANs; normal WPA2 passphrases or a 64-hex PSK.
  bool validPsk=true;
  if (len==64) for (char c:next.password) if (c && hex(c)<0) validPsk=false;
  if (!ok || (len && len<8) || !validPsk) { Serial.println("MS3WIFI1 ERROR CREDENTIALS"); return; }
  Preferences prefs;
  if (!prefs.begin("moonlight",false,storage::Partition)) { Serial.println("MS3WIFI1 ERROR STORAGE"); return; }
  const bool saved=prefs.putBytes("wifi",&next,sizeof(next))==sizeof(next);
  prefs.end();
  if (!saved) { Serial.println("MS3WIFI1 ERROR STORAGE"); return; }
  Serial.println("MS3WIFI1 OK RESTART");
  // Passwords are never echoed. Reboot only in response to this explicit USB command.
  memset(&next,0,sizeof(next)); memset(line,0,sizeof(line));
  Serial.flush(); delay(150); ESP.restart();
}
void serialPoll() {
  // Bounded work; a partial/malformed command never blocks video/touch.
  for (int count=0;count<64 && Serial.available();++count) {
    const char c=Serial.read();
    if (c=='\r') continue;
    if (c=='\n') {
      if (!overflow) { line[lineSize]=0; command(); }
      else Serial.println("MS3WIFI1 ERROR LENGTH");
      memset(line,0,sizeof(line)); lineSize=0; overflow=false;
    } else if (!overflow) {
      if (!c || lineSize>=sizeof(line)-1) overflow=true;
      else line[lineSize++]=c;
    }
  }
}
void advertise() {
  if (!discovery.begin(config::DiscoveryPort)) Serial.println("LAN discovery bind failed");
  mdns=MDNS.begin(hostname);
  if (mdns) {
    MDNS.addService("moonlight-s3","tcp",config::Port);
    MDNS.addServiceTxt("moonlight-s3","tcp","id",static_cast<const char *>(id));
    MDNS.addServiceTxt("moonlight-s3","tcp","version","1");
  }
}
void hostAnnouncement(char *request) {
  char *f[13]={},*save=nullptr;int count=0;
  for(char *part=strtok_r(request," ",&save);part&&count<13;part=strtok_r(nullptr," ",&save))f[count++]=part;
  const bool modern=count==12&&!strcmp(f[0],"MS3HOST3");
  const bool legacy=count==8&&!strcmp(f[0],"MS3HOST2");
  if((!modern&&!legacy)||strlen(f[1])!=32||strlen(f[2])!=32||strcmp(f[3],id)||
     (strcmp(f[4],"0")&&strcmp(f[4],"1"))||strlen(f[5])>96)return;
  for(int i=0;i<32;++i)if(hex(f[1][i])<0||hex(f[2][i])<0)return;
  if(modern&&((strcmp(f[6],"I")&&strcmp(f[6],"B"))||strlen(f[8])!=8))return;
  char name[49]={};if(!decodeHex(f[5],name,sizeof(name))||!name[0])return;
  for(const unsigned char *c=(const unsigned char *)name;*c;++c)if(*c<32||*c==127)return;
  char prefix[320],expected[65];
  const int length=modern?snprintf(prefix,sizeof(prefix),"MS3HOST3 %s %s %s %s %s %s %s %s %s %s",f[1],f[2],id,f[4],f[5],f[6],f[7],f[8],f[9],f[10]):
    snprintf(prefix,sizeof(prefix),"MS3HOST2 %s %s %s %s %s 1",f[1],f[2],id,f[4],f[5]);
  pairing::signature(f[2],prefix,length,expected);const char *signature=f[modern?11:7];bool trusted=false;
  if(!strcmp(f[4],"1")&&expected[0]&&strlen(signature)==64){uint8_t d=0;for(int i=0;i<64;++i)d|=expected[i]^signature[i];trusted=!d;}
  int index=-1;for(int i=0;i<computersCount;++i)if(!strcmp(computers[i].id,f[2])){index=i;break;}
  if(index<0&&computersCount<8)index=computersCount++;
  if(index>=0){auto &c=computers[index];const String ip=discovery.remoteIP().toString();bool busy=modern&&!strcmp(f[6],"B");
    if(strcmp(c.id,f[2])||strcmp(c.name,name)||strcmp(c.ip,ip.c_str())||c.paired!=trusted||c.busy!=busy||!c.online)++revision;
    strlcpy(c.id,f[2],sizeof(c.id));strlcpy(c.name,name,sizeof(c.name));strlcpy(c.ip,ip.c_str(),sizeof(c.ip));
    c.paired=trusted;c.busy=busy;c.online=true;c.saved=pairing::paired(c.id);c.seen=millis();
    c.monitors=modern?constrain(atoi(f[10]),1,8):1;
  }
  const bool chosen=playId&&!strcmp(selectedComputer,f[2]);
  if(chosen&&trusted)pinRequired=false;
  if(modern&&chosen&&strtoul(f[8],nullptr,16)==playId&&(trusted||pinRequired)){
    const int phase=atoi(f[7]);
    if(playing!=PlayState::Cancelled&&playing!=PlayState::Disconnected&&playing!=PlayState::Declined&&playing!=PlayState::Failed&&playing!=PlayState::TimedOut&&
       (playing!=PlayState::Streaming||phase>=7)){
      reason=constrain(atoi(f[9]),0,9);
      switch(phase){case 1:playing=PlayState::Accepted;break;case 2:playing=PlayState::Pin;break;case 3:playing=PlayState::Verifying;break;
        case 4:playing=PlayState::Preparing;break;case 5:if(playing!=PlayState::Streaming)playing=PlayState::FirstFrame;break;
        case 7:playing=PlayState::Declined;break;case 8:playing=PlayState::Failed;break;
        case 9:playing=PlayState::Disconnected;break;case 10:playing=PlayState::Cancelled;break;}
    }
  }
  const bool authorized=trusted&&(!chosen||!pinRequired||playing==PlayState::Streaming||playing==PlayState::FirstFrame);
  const unsigned flow=playing==PlayState::Cancelled?3:playing==PlayState::Streaming?2:
    (playing==PlayState::Declined||playing==PlayState::Failed||playing==PlayState::Disconnected||playing==PlayState::TimedOut)?4:1;
  char response[256];const int n=modern?snprintf(response,sizeof(response),"MS3DEV3 %s %s %s %08lx %d %08lx %u",f[1],f[2],id,chosen?playId:0UL,authorized?1:0,options(f[2]),chosen?flow:0):
    snprintf(response,sizeof(response),"MS3DEV2 %s %s %s %08lx %d",f[1],f[2],id,chosen?playId:0UL,authorized?1:0);
  char mac[65]={};if(authorized)pairing::signature(f[2],response,n,mac);
  snprintf(response+n,sizeof(response)-n," %s",mac[0]?mac:"-");
  if(discovery.beginPacket(discovery.remoteIP(),discovery.remotePort())){discovery.write((const uint8_t *)response,strlen(response));discovery.endPacket();}
}
void discoverPoll() {
  const int size=discovery.parsePacket();
  if (!size) return;
  char request[384]={};
  if(size>0&&size<int(sizeof(request))&&size!=41) {
    if(discovery.read(request,size)==size&&(!strncmp(request,"MS3HOST2 ",9)||!strncmp(request,"MS3HOST3 ",9)))hostAnnouncement(request);
    else discovery.clear();
    return;
  }
  if (size!=41 || discovery.read(request,41)!=41 || strncmp(request,"MS3DISC1 ",9)) {
    discovery.clear(); return;
  }
  for (int i=9;i<41;++i) if (hex(request[i])<0) return;
  // Bound response rate even during malformed or excessive broadcast traffic.
  if (millis()-lastReply<20) return;
  lastReply=millis();
  char response[128];
  const int n=snprintf(response,sizeof(response),"MS3HERE1 %s %s %u %s",request+9,id,config::Port,hostname);
  if (discovery.beginPacket(discovery.remoteIP(),discovery.remotePort())) {
    discovery.write(reinterpret_cast<const uint8_t *>(response),n); discovery.endPacket();
  }
}
}
void begin() {
  WiFi.onEvent([](arduino_event_id_t event,arduino_event_info_t) {
    if (event==ARDUINO_EVENT_WIFI_STA_CONNECTED) associations.fetch_add(1,std::memory_order_relaxed);
  });
  WiFi.persistent(false); // App credentials live in our dedicated NVS, not the shared Wi-Fi store.
  WiFi.mode(WIFI_STA); // Never creates a SoftAP, including provisioning/recovery.
  const String mac=WiFi.macAddress();
  String compact=mac; compact.replace(":",""); compact.toLowerCase();
  strlcpy(id,compact.c_str(),sizeof(id));
  snprintf(hostname,sizeof(hostname),"moonlight-s3-%s",id+6);
  WiFi.setHostname(hostname); WiFi.setSleep(false); WiFi.setAutoReconnect(true);
  strlcpy(credentials.ssid,config::WifiSsid,sizeof(credentials.ssid));
  strlcpy(credentials.password,config::WifiPassword,sizeof(credentials.password));
  Preferences prefs;
  if (prefs.begin("moonlight",true,storage::Partition)) {
    Credentials stored={};
    if (prefs.getBytesLength("wifi")==sizeof(stored) &&
        prefs.getBytes("wifi",&stored,sizeof(stored))==sizeof(stored) &&
        stored.ssid[0] && memchr(stored.ssid,0,sizeof(stored.ssid)) &&
        memchr(stored.password,0,sizeof(stored.password))) credentials=stored;
    prefs.end();
  }
  if (credentials.ssid[0]) WiFi.begin(credentials.ssid,credentials.password);
  else Serial.println("LAN: configure WiFi using USB tool; no hotspot is created");
  Preferences uiPrefs;if(uiPrefs.begin("ms3ui",true,storage::Partition)){Profiles stored;if(uiPrefs.getBytesLength("profiles")==sizeof(stored)&&uiPrefs.getBytes("profiles",&stored,sizeof(stored))==sizeof(stored)&&stored.version==1)profiles=stored;uiPrefs.end();}
  lastRetry=millis();
}
bool connected() { return online; }
const char *name() { return hostname; }
String ssid() { return connected()?WiFi.SSID():String(credentials.ssid); }
JoinState joinState() { return joining; }
void setConfiguring(bool enabled) {
  configuring=enabled;
  WiFi.setAutoReconnect(!enabled);
  if (enabled) {
    cancelPlay();
    joining=JoinState::Idle;
    if (WiFi.status()!=WL_CONNECTED) WiFi.disconnect(false,false);
  } else {
    const bool restore=joining==JoinState::Connecting || joining==JoinState::Failed || WiFi.status()!=WL_CONNECTED;
    joining=JoinState::Idle;
    memset(&candidate,0,sizeof(candidate));
    if (restore) {
      WiFi.disconnect(false,false);
      if (credentials.ssid[0]) WiFi.begin(credentials.ssid,credentials.password);
      lastRetry=millis();
    }
  }
}
PlayState playState() { return playing; }
void scanComputers(){for(int i=0;i<computersCount;++i)computers[i].seen=0; ++revision;}
int computerCount(){return computersCount;}
bool computer(int index,Computer &out){if(index<0||index>=computersCount)return false;out=computers[index];out.paired=out.paired&&pairing::paired(out.id);return true;}
uint32_t computerRevision(){return revision;}
const char *selectedName(){return selectedComputerName;}
bool needsPin(){return pinRequired;}
void cancelPlay(){playing=PlayState::Cancelled;reason=8;pairing::cancel();}
const char *selectedId(){return selectedComputer;}
uint32_t requestId(){return playId;}
uint32_t options(const char *host){host=host?host:selectedComputer;for(const auto &p:profiles.entries)if(!strcmp(p.host,host))return p.value;return DefaultOptions;}
bool saveOptions(const char *host,uint32_t value){int index=-1;for(int i=0;i<8;++i)if(!strcmp(profiles.entries[i].host,host)){index=i;break;}
  if(index<0)for(int i=0;i<8;++i)if(!profiles.entries[i].host[0]){index=i;break;}if(index<0)return false;
  Profiles next=profiles;strlcpy(next.entries[index].host,host,33);next.entries[index].value=value;
  Preferences p;if(!p.begin("ms3ui",false,storage::Partition))return false;bool ok=p.putBytes("profiles",&next,sizeof(next))==sizeof(next);p.end();if(ok)profiles=next;return ok;
}
const char *message(){if(failure[0])return failure;switch(playing){
  case PlayState::Waiting:return "等待电脑响应";case PlayState::Accepted:return "电脑已响应";case PlayState::Pin:return pairing::status();
  case PlayState::Verifying:return "正在验证配对";case PlayState::Preparing:return "正在准备桌面";case PlayState::FirstFrame:return "等待桌面首帧";
  case PlayState::Streaming:return localPaused?"投屏已暂停，电脑桌面保持原状":"正在串流";case PlayState::Declined:return "电脑已取消配对，可点击重试";
  case PlayState::Cancelled:return "已取消连接";case PlayState::TimedOut:return "电脑未响应，请检查软件是否运行";
  case PlayState::Failed:case PlayState::Disconnected:switch(reason){case 2:return "授权验证失败，请重新确认配对";case 3:return "电脑正被其他设备使用";case 4:return "电脑抓屏失败，请检查显示器";case 5:return "电脑拒绝输入，请检查权限";case 6:return "画面解码失败，可重连";case 7:return "配对已过期，请重试";case 8:return "电脑已停止投屏";case 9:return "电脑软件与设备版本不匹配";default:return "网络已断开，可重新连接";}
  default:return "选择电脑开始串流";}}
bool authorizeRequest(const char *host,uint32_t request){return request&&request==playId&&!strcmp(host,selectedComputer)&&
  playing!=PlayState::Cancelled&&playing!=PlayState::Declined&&playing!=PlayState::Failed&&playing!=PlayState::Disconnected&&playing!=PlayState::TimedOut;}
void setLocalPause(bool value){localPaused=value;}
bool requestPlay(const char *computerId) {
  char target[33];strlcpy(target,computerId,sizeof(target));strlcpy(selectedComputer,target,sizeof(selectedComputer));
  failure="";if (!online || configuring){failure="请先连接 Wi-Fi";return false;}
  int index=-1;for(int i=0;i<computersCount;++i)if(!strcmp(computers[i].id,computerId)&&millis()-computers[i].seen<10000){index=i;break;}
  if(index<0){failure="电脑已离线，请检查软件是否运行";return false;}
  const auto &c=computers[index];
  if(c.busy){failure="电脑正被其他设备使用，请稍后重试";return false;}strlcpy(selectedComputer,c.id,sizeof(selectedComputer));strlcpy(selectedComputerName,c.name,sizeof(selectedComputerName));
  pinRequired=!(c.paired&&pairing::paired(c.id));
  if(pinRequired){pairing::open(c.id,c.name);if(!pairing::enabled()){failure=pairing::status();return false;}}else pairing::cancel();
  do { playId=esp_random(); } while (!playId);
  playStarted=millis();reason=0;localPaused=false;playing=PlayState::Waiting;
  return true;
}
void streamStarted(){playing=PlayState::FirstFrame;pinRequired=false;localPaused=false;}
void frameShown(){if(playing==PlayState::FirstFrame)playing=PlayState::Streaming;}
void streamEnded(uint32_t error){if(playing!=PlayState::Cancelled&&playing!=PlayState::Declined&&playing!=PlayState::Failed&&playing!=PlayState::Disconnected&&playing!=PlayState::TimedOut){reason=error;playing=PlayState::Disconnected;}}
bool join(const char *ssid,const char *password) {
  const size_t a=strlen(ssid),b=strlen(password);
  if (!configuring || !a || a>32 || b>64 || (b && b<8)) return false;
  if (b==64) for (size_t i=0;i<b;++i) if (hex(password[i])<0) return false;
  memset(&candidate,0,sizeof(candidate));
  strlcpy(candidate.ssid,ssid,sizeof(candidate.ssid));
  strlcpy(candidate.password,password,sizeof(candidate.password));
  joining=JoinState::Connecting; joinStarted=millis();
  joinAssociation=associations.load(std::memory_order_relaxed);
  WiFi.disconnect(false,false);
  WiFi.begin(candidate.ssid,candidate.password);
  return true;
}
String status() {
  if (online) return address.toString();
  wifi_ap_record_t ap={};
  if (esp_wifi_sta_get_ap_info(&ap)==ESP_OK) return "WiFi joined - waiting DHCP";
  return credentials.ssid[0]?"Joining WiFi - retrying...":"Set WiFi with USB tool";
}
bool poll() {
  serialPoll();
  if(playId&&playing!=PlayState::Streaming&&playing!=PlayState::Cancelled&&playing!=PlayState::Disconnected&&playing!=PlayState::Declined&&playing!=PlayState::Failed&&
      millis()-playStarted>(pinRequired?120000U:playing==PlayState::Waiting?15000U:60000U)){playing=PlayState::TimedOut;pairing::cancel();}
  for(int p=0;p<pairing::count();++p){const char *host=pairing::peerId(p);bool found=false;for(int i=0;i<computersCount;++i)if(!strcmp(computers[i].id,host)){computers[i].saved=true;found=true;break;}
    if(!found&&computersCount<8){auto &c=computers[computersCount++];c=Computer{};strlcpy(c.id,host,33);strlcpy(c.name,pairing::peerName(p),49);c.saved=true;++revision;}}
  for(int i=0;i<computersCount;){auto &c=computers[i];c.saved=pairing::paired(c.id);if(c.online&&(!c.seen||millis()-c.seen>10000)){c.online=false;c.busy=false;++revision;}
    if(!c.online&&!c.saved){for(int j=i+1;j<computersCount;++j)computers[j-1]=computers[j];--computersCount;++revision;}else ++i;}
  const bool now=WiFi.status()==WL_CONNECTED && uint32_t(WiFi.localIP())!=0;
  const IPAddress ip=now?WiFi.localIP():IPAddress();
  const bool changed=now!=online || ip!=address;
  if (joining==JoinState::Connecting) {
    if (now && associations.load(std::memory_order_relaxed)!=joinAssociation && WiFi.SSID()==candidate.ssid) {
      Preferences prefs;
      bool saved=false;
      if (prefs.begin("moonlight",false,storage::Partition)) {
        saved=prefs.putBytes("wifi",&candidate,sizeof(candidate))==sizeof(candidate);
        prefs.end();
      }
      if (saved) credentials=candidate;
      joining=saved?JoinState::Saved:JoinState::Failed;
      memset(&candidate,0,sizeof(candidate));
      Serial.println(saved?"WiFi setup: connected and saved":"WiFi setup: NVS save failed");
    } else if (millis()-joinStarted>20000) {
      WiFi.disconnect(false,false); joining=JoinState::Failed;
      memset(&candidate,0,sizeof(candidate));
      Serial.println("WiFi setup: connection timeout; saved credentials preserved");
    }
  }
  if (changed) {
    discovery.stop(); if (mdns) MDNS.end(); mdns=false;
    online=now; address=ip;
    if (online) advertise();
    Serial.printf("LAN: %s %s\n",hostname,status().c_str());
  }
  if (!online && !configuring && credentials.ssid[0] && millis()-lastRetry>=15000) {
    lastRetry=millis(); WiFi.reconnect();
  }
  if (online) discoverPoll();
  return changed;
}
}
