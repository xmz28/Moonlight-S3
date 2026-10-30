#include <Arduino.h>
#include <WiFi.h>
#include <NetworkUdp.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include "config.h"
#include "lan.h"

namespace lan {
namespace {
struct Credentials { char ssid[33]; char password[65]; } credentials={};
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
  if (!prefs.begin("moonlight",false)) { Serial.println("MS3WIFI1 ERROR STORAGE"); return; }
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
void discoverPoll() {
  const int size=discovery.parsePacket();
  if (!size) return;
  char request[42]={};
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
  WiFi.mode(WIFI_STA); // Never creates a SoftAP, including provisioning/recovery.
  const String mac=WiFi.macAddress();
  String compact=mac; compact.replace(":",""); compact.toLowerCase();
  strlcpy(id,compact.c_str(),sizeof(id));
  snprintf(hostname,sizeof(hostname),"moonlight-s3-%s",id+6);
  WiFi.setHostname(hostname); WiFi.setSleep(false); WiFi.setAutoReconnect(true);
  strlcpy(credentials.ssid,config::WifiSsid,sizeof(credentials.ssid));
  strlcpy(credentials.password,config::WifiPassword,sizeof(credentials.password));
  Preferences prefs;
  if (prefs.begin("moonlight",true)) {
    Credentials stored={};
    if (prefs.getBytesLength("wifi")==sizeof(stored) &&
        prefs.getBytes("wifi",&stored,sizeof(stored))==sizeof(stored) &&
        stored.ssid[0] && memchr(stored.ssid,0,sizeof(stored.ssid)) &&
        memchr(stored.password,0,sizeof(stored.password))) credentials=stored;
    prefs.end();
  }
  if (credentials.ssid[0]) WiFi.begin(credentials.ssid,credentials.password);
  else Serial.println("LAN: configure WiFi using USB tool; no hotspot is created");
  lastRetry=millis();
}
bool connected() { return online; }
const char *name() { return hostname; }
String status() {
  if (online) return address.toString();
  wifi_ap_record_t ap={};
  if (esp_wifi_sta_get_ap_info(&ap)==ESP_OK) return "WiFi joined - waiting DHCP";
  return credentials.ssid[0]?"Joining WiFi - retrying...":"Set WiFi with USB tool";
}
bool poll() {
  serialPoll();
  const bool now=WiFi.status()==WL_CONNECTED && uint32_t(WiFi.localIP())!=0;
  const IPAddress ip=now?WiFi.localIP():IPAddress();
  const bool changed=now!=online || ip!=address;
  if (changed) {
    discovery.stop(); if (mdns) MDNS.end(); mdns=false;
    online=now; address=ip;
    if (online) advertise();
    Serial.printf("LAN: %s %s\n",hostname,status().c_str());
  }
  if (!online && credentials.ssid[0] && millis()-lastRetry>=15000) {
    lastRetry=millis(); WiFi.reconnect();
  }
  if (online) discoverPoll();
  return changed;
}
}
