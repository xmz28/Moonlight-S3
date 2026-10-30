#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_partition.h>
#include "ui.h"
#include "display.h"
#include "lan.h"
#include "pairing.h"

namespace ui {
namespace {
const esp_partition_t *fontPartition=nullptr;
constexpr uint32_t GlyphCount=21439;
constexpr uint32_t WidthOffset=16+(GlyphCount+1)*4;
constexpr uint32_t PixelOffset=WidthOffset+GlyphCount;
struct GlyphInfo { uint32_t start=0,end=0;uint8_t advance=22; };
struct GlyphCache { int glyph=-1;GlyphInfo info; } glyphCache[16];
uint32_t cacheNext=0;
uint8_t glyphBitmap[1024];
enum class Page { Home, Networks, Keyboard, Joining, Pairing, Paired, Result, Menu, Options, Text, Forget };
Page page=Page::Home;
struct Network { char ssid[33]; int rssi; wifi_auth_mode_t auth; } networks[20];
int count=0,offset=0;
bool dirty=true,scanning=false,requestScan=false,wasDown=false,armed=false;
bool uppercase=false,symbols=false,showPassword=false,editingSsid=false,openNetwork=false;
char selected[33]={},password[65]={};
String notice;
uint32_t scanStarted=0;
lan::JoinState lastJoin=lan::JoinState::Idle;
lan::PlayState lastPlay=lan::PlayState::Idle;
String lastPair;
int deleteIndex=-1,computerOffset=0;
uint32_t lastComputerRevision=0,editedOptions=0,actionNumber=0;
char optionsHost[33]={},inputText[129]={};
char forgetHost[33]={},forgetName[49]={};
bool optionsFromMenu=false;
Action pendingAction=Action::None;
String lastMessage;
uint16_t *canvas=nullptr;
constexpr uint16_t Background=0x08c4,Card=0x1127,Line=0x21ed,White=0xef9e,Muted=0x8c55,Accent=0x3e78,Green=0x5e8f;

uint32_t nextChar(const char *&s) {
  const uint8_t a=*s++;
  if (a<128) return a;
  int n=0; uint32_t code=0;
  if ((a&0xe0)==0xc0) { n=1;code=a&31; }
  else if ((a&0xf0)==0xe0) { n=2;code=a&15; }
  else if ((a&0xf8)==0xf0) { n=3;code=a&7; }
  else return 0x25a1;
  for (int i=0;i<n;++i) {
    const uint8_t b=*s;
    if ((b&0xc0)!=0x80) return 0x25a1;
    ++s; code=(code<<6)|(b&63);
  }
  return code;
}
int glyph(uint32_t code,bool title) {
  if (title) return code>=32 && code<=126?code-32:'?'-32;
  int base=95;
  constexpr uint32_t ranges[][2]={{32,126},{160,255},{0x3000,0x303f},{0x4e00,0x9fff},{0xff01,0xff60},{0x25a1,0x25a1}};
  for (auto &r:ranges) { if (code>=r[0] && code<=r[1]) return base+code-r[0]; base+=r[1]-r[0]+1; }
  return base-1;
}
GlyphInfo glyphInfo(int g) {
  for (const auto &entry:glyphCache) if (entry.glyph==g) return entry.info;
  GlyphInfo info;uint32_t offsets[2]={};
  if (esp_partition_read(fontPartition,16+g*4,offsets,sizeof(offsets))==ESP_OK &&
      esp_partition_read(fontPartition,WidthOffset+g,&info.advance,1)==ESP_OK &&
      offsets[1]>=offsets[0] && offsets[1]-offsets[0]<=sizeof(glyphBitmap) &&
      offsets[1]<=fontPartition->size-PixelOffset) {
    info.start=PixelOffset+offsets[0];info.end=PixelOffset+offsets[1];
  }
  glyphCache[cacheNext%16]={g,info};++cacheNext;
  return info;
}
int textWidth(const char *s,bool title=false) {
  int w=0;
  while (*s) w+=glyphInfo(glyph(nextChar(s),title)).advance;
  return w;
}
void rectangle(int x,int y,int w,int h,uint16_t color) {
  for (int yy=max(0,y);yy<min(480,y+h);++yy)
    for (int xx=max(0,x);xx<min(800,x+w);++xx) canvas[yy*800+xx]=color;
}
uint16_t blend(uint16_t a,uint16_t b,int alpha) {
  if (alpha==3) return a;
  return ((((a>>11)*alpha+(b>>11)*(3-alpha))/3)<<11)|
    (((((a>>5)&63)*alpha+((b>>5)&63)*(3-alpha))/3)<<5)|
    (((a&31)*alpha+(b&31)*(3-alpha))/3);
}
void text(int x,int y,const char *s,uint16_t color=White,bool title=false,int right=776) {
  const int width=title?40:24,height=title?44:30;
  while (*s) {
    const int g=glyph(nextChar(s),title);
    const GlyphInfo info=glyphInfo(g);const int advance=info.advance;
    if (x+advance>right) break;
    const size_t size=info.end-info.start;
    if (!size || esp_partition_read(fontPartition,info.start,glyphBitmap,size)!=ESP_OK) { x+=advance;continue; }
    const uint8_t *p=glyphBitmap,*end=p+size;
    int pixel=0;
    while (p<end && pixel<width*height) {
      const uint8_t packed=*p++;
      if (!packed) { if (p>=end) break; pixel+=int(*p++)*4;continue; }
      for (int i=0;i<4;++i,++pixel) {
        const int alpha=(packed>>(6-i*2))&3,xx=x+pixel%width,yy=y+pixel/width;
        if (alpha && xx>=0 && xx<right && yy>=0 && yy<480)
          canvas[yy*800+xx]=blend(color,canvas[yy*800+xx],alpha);
      }
    }
    x+=advance;
  }
}
bool hit(int x,int y,int w,int h,const touch::State &t) { return t.x>=x && t.x<x+w && t.y>=y && t.y<y+h; }
void button(int x,int y,int w,int h,const char *label,bool accent=false) {
  rectangle(x,y,w,h,accent?Accent:Line);
  text(x+(w-textWidth(label))/2,y+(h-30)/2,label,accent?Background:White);
}
void heading(const char *title,const char *subtitle) {
  rectangle(0,0,800,480,Background);
  text(24,17,title,White,true); text(26,65,subtitle,Muted);
}
void cancelScan() {
  if (scanning) esp_wifi_scan_stop();
  WiFi.scanDelete();
  scanning=requestScan=false;
}
void home() {
  cancelScan();
  memset(password,0,sizeof(password));memset(selected,0,sizeof(selected));
  notice="";page=Page::Home;lan::setConfiguring(false);dirty=true;
}
void startScan() {
  cancelScan(); count=offset=0;notice="正在扫描 2.4 GHz Wi-Fi...";
  const int result=WiFi.scanNetworks(true,false,false,120);
  scanning=result==WIFI_SCAN_RUNNING;scanStarted=millis();
  if (!scanning) notice="扫描未启动，请点重新扫描";
  dirty=true;
}
void collectScan() {
  const int found=WiFi.scanComplete();
  if (found==WIFI_SCAN_RUNNING && millis()-scanStarted<10000) return;
  if (found<0) { cancelScan();notice="扫描失败，请重新扫描";dirty=true;return; }
  count=0;
  for (int i=0;i<found;++i) {
    const String ssid=WiFi.SSID(i);
    if (!ssid.length()) continue;
    const int strength=WiFi.RSSI(i);
    bool duplicate=false;
    for (int j=0;j<count;++j) if (ssid==networks[j].ssid) {
      duplicate=true;
      if (strength>networks[j].rssi) { networks[j].rssi=strength;networks[j].auth=WiFi.encryptionType(i); }
      break;
    }
    if (duplicate) continue;
    int place=count;
    if (count==20) {
      place=0; for (int j=1;j<count;++j) if (networks[j].rssi<networks[place].rssi) place=j;
      if (strength<=networks[place].rssi) continue;
    } else ++count;
    strlcpy(networks[place].ssid,ssid.c_str(),sizeof(networks[place].ssid));
    networks[place].rssi=strength;networks[place].auth=WiFi.encryptionType(i);
  }
  for (int i=0;i<count;++i) for (int j=i+1;j<count;++j)
    if (networks[j].rssi>networks[i].rssi) { auto temp=networks[i];networks[i]=networks[j];networks[j]=temp; }
  WiFi.scanDelete();scanning=false;
  notice=count?"轻触网络名称以连接":"没有找到网络，请重扫或手动输入";
  Serial.printf("WiFi UI: scan complete, %d unique networks\n",count);
  dirty=true;
}
void connect() {
  if (!selected[0]) { notice="请输入 Wi-Fi 名称";dirty=true;return; }
  if (!openNetwork && !password[0]) { notice="请输入此网络的密码";dirty=true;return; }
  if (!lan::join(selected,password)) { notice="密码需为 8-63 字符或 64 位十六进制";dirty=true;return; }
  page=Page::Joining;lastJoin=lan::JoinState::Connecting;notice="";dirty=true;
  memset(password,0,sizeof(password));
}
const char *keys(int row) {
  static const char *lower[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
  static const char *upper[]={"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
  static const char *punct[]={"!@#$%^&*()","-_=+[]{}\\|",";:'\",.<>/?","`~"};
  return symbols?punct[row]:uppercase?upper[row]:lower[row];
}
void draw() {
  const uint32_t drawStarted=micros();
  int index=-1;canvas=display::beginUi(index);if (!canvas) return;
  if (page==Page::Home) {
    heading("Moonlight S3","选择电脑，即可开始串流");button(432,24,162,52,"已配对设备");button(610,24,166,52,"Wi-Fi 设置");
    const String network=lan::connected()?lan::ssid()+" · "+lan::status():"请先连接 Wi-Fi";
    text(26,97,network.c_str(),Muted,false,775);
    if(!lan::computerCount()) {
      text(48,166,"正在查找局域网电脑...",White);
      text(48,218,"请保持电脑上的 Moonlight S3 软件打开",Muted);
    }
    for(int row=0;row<4;++row){
      lan::Computer c;if(!lan::computer(computerOffset+row,c))break;
      const int y=134+row*64;rectangle(24,y,752,58,Card);
      text(40,y+1,c.name,White,false,400);text(40,y+29,c.online?c.ip:"已保存的电脑",Muted);
      text(416,y+12,!c.online?"离线":c.busy?"正在使用":c.paired?"已配对":c.saved?"授权需确认":"首次需配对",!c.online?Muted:c.paired?Green:Accent);
      button(670,y+6,94,46,"设置");
    }
    button(24,410,180,48,"刷新电脑");
    if(lan::computerCount()>4){button(464,410,120,48,"上一页");button(600,410,120,48,"下一页");}
  } else if (page==Page::Pairing) {
    heading("Connecting",lan::selectedName());button(656,24,120,52,"返回");
    rectangle(24,120,752,218,Card);
    const auto phase=lan::playState();
    const bool timeout=phase==lan::PlayState::TimedOut||phase==lan::PlayState::Declined||phase==lan::PlayState::Failed||phase==lan::PlayState::Disconnected||phase==lan::PlayState::Cancelled;
    text(48,145,lan::message(),White);
    if(lan::needsPin()&&!timeout)text(300,207,pairing::pin(),Accent,true);
    text(48,288,lan::needsPin()?"在电脑弹窗输入此 PIN，成功后自动串流":"以后点击这台电脑即可直接串流",Muted);
    if(timeout)button(24,364,200,58,"重新连接",true);
  } else if(page==Page::Result){
    heading("Connection",lan::selectedName());button(656,24,120,52,"返回");
    rectangle(24,120,752,220,Card);text(48,150,lan::message(),Accent);
    text(48,220,"原有配对和电脑桌面会保留",Muted);
    button(24,364,200,58,"重新连接",true);button(248,364,200,58,"返回列表");
  } else if(page==Page::Menu){
    heading("Stream menu","本地菜单 · 投屏暂停，电脑上的程序继续运行");button(656,24,120,52,"继续");
    button(24,122,176,58,"输入文字");button(216,122,176,58,(lan::options()&1)?"切换直接触摸":"切换触摸板");button(408,122,176,58,"电脑设置");button(600,122,176,58,(lan::options()&2)?"浮窗：开启":"浮窗：关闭");
    const char *labels[]={"Esc","Enter","Tab","Backspace","Ctrl+C","Ctrl+V","Ctrl+A","Alt+Tab"};
    for(int i=0;i<8;++i)button(24+(i%4)*192,222+(i/4)*66,176,54,labels[i]);
    button(24,390,224,58,"断开并返回");button(272,390,244,58,"重新连接");button(540,390,236,58,"继续桌面",true);
  } else if(page==Page::Options){
    heading("Computer settings","常用设置保存在这台 S3，按电脑分别记忆");button(656,24,120,52,"返回");
    char label[64];
    button(24,130,360,54,(editedOptions&1)?"输入：单指触摸板":"输入：直接触摸");
    snprintf(label,sizeof(label),"显示器：%lu",(editedOptions>>8)&15);button(416,130,360,54,label);
    button(24,194,360,54,(editedOptions&16)?"画面：流畅 400×240":"画面：清晰 800×480");
    button(416,194,360,54,(editedOptions&4)?"底栏：显示":"底栏：隐藏");
    button(24,258,360,54,(editedOptions&8)?"操作权限：允许触控":"操作权限：仅观看");
    snprintf(label,sizeof(label),"JPEG 质量：%lu",(editedOptions>>16)&255);button(416,258,360,54,label);
    button(24,322,360,54,(editedOptions&2)?"性能浮窗：开启":"性能浮窗：关闭");
    snprintf(label,sizeof(label),"目标帧率：%lu FPS",(editedOptions>>24)&63);button(416,322,360,54,label);
    button(24,406,360,54,optionsFromMenu?"保存并重新连接":"保存并返回",true);
    if(notice.length())text(24,98,notice.c_str(),Accent);
    text(416,418,"单指长按右键，无需双指",Muted);
  } else if(page==Page::Forget){
    heading("Forget computer","仅删除授权，不会隐藏局域网里仍在线的电脑");
    text(36,138,forgetName,White);text(36,190,forgetHost,Muted);
    text(36,248,"下次连接这台电脑，需要重新输入屏上 PIN",Muted);
    if(notice.length())text(36,302,notice.c_str(),Accent);
    button(24,390,360,58,"确认删除",true);button(408,390,368,58,"取消");
  } else if (page==Page::Paired) {
    heading("Paired devices",notice.length()?notice.c_str():"删除后，下次串流需要重新配对");button(656,24,120,52,"返回");
    if(!pairing::count())text(48,166,"暂无已配对电脑",Muted);
    for(int i=0;i<pairing::count();++i){const int y=128+i*76;
      rectangle(24,y,752,66,Card);text(40,y+2,pairing::peerName(i),White,false,620);
      text(40,y+32,pairing::peerId(i),Muted,false,620);
      button(642,y+8,120,48,deleteIndex==i?"确认删除":"删除");
    }
  } else if (page==Page::Networks) {
    heading("Wi-Fi","只在此页面手动扫描；串流时不扫描");button(656,24,120,52,"返回");
    text(24,101,notice.c_str(),scanning?Accent:Muted);
    for (int i=0;i<4 && offset+i<count;++i) {
      const Network &n=networks[offset+i];const int y=145+i*56;
      rectangle(24,y,752,50,Card);text(40,y+8,n.ssid,White,false,610);
      char info[32];snprintf(info,sizeof(info),"%d dBm %s",n.rssi,n.auth==WIFI_AUTH_OPEN?"OPEN":"LOCK");
      text(620,y+8,info,Muted,false,780);
    }
    button(24,402,166,54,scanning?"扫描中...":"重新扫描");button(202,402,166,54,"手动输入");
    button(460,402,112,54,"上一页");button(584,402,112,54,"下一页");
    char pages[24];snprintf(pages,sizeof(pages),"%d/%d",offset/4+1,max(1,(count+3)/4));text(712,414,pages,Muted);
  } else if (page==Page::Keyboard||page==Page::Text) {
    heading(page==Page::Text?"Text input":editingSsid?"Network name":"Wi-Fi password",editingSsid?"输入 Wi-Fi 名称，然后输入密码":page==Page::Text?"英文、数字、符号；在电脑当前光标位置输入":selected);
    rectangle(24,103,752,44,Card);
    if(page==Page::Text){const char *value=inputText;while(textWidth(value)>720&&*value)++value;text(38,110,value);}
    else if (editingSsid) text(38,110,selected);
    else {
      char hidden[65]={};memset(hidden,'*',min<size_t>(strlen(password),40));
      const char *value=showPassword?password:hidden;
      // Keep the end of long ASCII passwords visible while editing.
      while (textWidth(value)>570 && *value) ++value;
      text(38,110,value);button(648,105,126,40,showPassword?"隐藏":"显示");
    }
    for (int row=0;row<4;++row) {
      const char *k=keys(row);const int n=strlen(k),left=(800-n*74)/2;
      for (int col=0;col<n;++col) { char label[2]={k[col],0};button(left+col*74,164+row*54,66,46,label); }
    }
    if (notice.length()) { rectangle(16,365,768,30,Background);text(24,365,notice.c_str(),Accent); }
    button(24,406,108,54,uppercase?"abc":"ABC");button(142,406,108,54,symbols?"字母":"符号");
    button(260,406,108,54,"空格");button(378,406,108,54,"退格");
    button(496,406,138,54,page==Page::Text?"发送":editingSsid?"下一步":"连接",true);button(644,406,132,54,page==Page::Text?"返回":"取消");
  } else {
    heading("Wi-Fi",selected);const auto state=lan::joinState();
    rectangle(24,124,752,208,Card);
    text(48,151,state==lan::JoinState::Connecting?"正在连接，请稍候...":state==lan::JoinState::Saved?"连接成功，设置已保存":"连接失败，原有设置已保留",state==lan::JoinState::Saved?Green:White);
    if (state==lan::JoinState::Saved) { const String ip=lan::status();text(48,214,ip.c_str(),White,true); }
    else text(48,222,"请确认密码、信号及 2.4 GHz 网络可用",Muted);
    button(24,402,200,54,"返回首页",true);
    if (state!=lan::JoinState::Connecting && state!=lan::JoinState::Saved) button(240,402,200,54,"重新输入密码");
  }
  display::endUi(index);canvas=nullptr;dirty=false;
  if(page==Page::Menu||page==Page::Text||(page==Page::Options&&optionsFromMenu))Serial.printf("UI local page=%u draw=%lu us\n",static_cast<unsigned>(page),micros()-drawStarted);
}
}
bool begin() {
  fontPartition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,static_cast<esp_partition_subtype_t>(0x40),"ui_font");
  uint32_t header[4]={};
  if (!fontPartition || esp_partition_read(fontPartition,0,header,sizeof(header))!=ESP_OK) return false;
  return !memcmp(header,"MSF1",4) && header[1]==GlyphCount;
}
bool active() { return page!=Page::Home && page!=Page::Pairing; }
void invalidate() { dirty=true; }
void streamEnded(){page=lan::playState()==lan::PlayState::Cancelled?Page::Home:Page::Result;dirty=true;}
void connecting(bool accepted){page=accepted?Page::Pairing:Page::Result;dirty=true;}
void openMenu(){page=Page::Menu;armed=false;wasDown=true;dirty=true;}
void openText(){openMenu();page=Page::Text;inputText[0]=0;symbols=uppercase=false;editingSsid=false;notice="";}
bool localMenu(){return page==Page::Menu||page==Page::Text||(page==Page::Options&&optionsFromMenu);}
Action takeAction(){auto a=pendingAction;pendingAction=Action::None;return a;}
const char *textValue(){return inputText;}
uint32_t actionValue(){return actionNumber;}
void poll(bool allowed) {
  if (!allowed) return; // No scan polling, allocation, text drawing or refresh during video.
  if (requestScan) { requestScan=false;startScan(); }
  if (scanning) collectScan();
  const auto state=lan::joinState();
  if (state!=lastJoin) { lastJoin=state;dirty=true; }
  const auto play=lan::playState();
  if (play!=lastPlay) { lastPlay=play;dirty=true; }
  const String pair=String(pairing::status())+pairing::pin();
  if(pair!=lastPair){lastPair=pair;dirty=true;}
  const auto revision=lan::computerRevision();
  if(revision!=lastComputerRevision){lastComputerRevision=revision;computerOffset=min(computerOffset,max(0,((lan::computerCount()-1)/4)*4));if(page==Page::Home)dirty=true;}
  if(page==Page::Pairing && lan::playState()==lan::PlayState::Idle && !lan::needsPin()){page=Page::Home;dirty=true;}
  const String status=lan::message();if(status!=lastMessage){lastMessage=status;dirty=true;}
  if (dirty) draw();
}
void touch(const touch::State &t) {
  if (!t.down) { wasDown=false;armed=true;return; }
  if (wasDown || !armed) return;
  wasDown=true;
  if (page==Page::Home) {
    if (hit(610,24,166,52,t)) {
      lan::cancelPlay();page=Page::Networks;lan::setConfiguring(true);requestScan=dirty=true;
    } else if(hit(432,24,162,52,t)){page=Page::Paired;deleteIndex=-1;notice="";dirty=true;}
    else if(hit(24,410,180,48,t)){lan::scanComputers();computerOffset=0;dirty=true;}
    else if(hit(464,410,120,48,t)){computerOffset=max(0,computerOffset-4);dirty=true;}
    else if(hit(600,410,120,48,t)){if(computerOffset+4<lan::computerCount())computerOffset+=4;dirty=true;}
    else for(int row=0;row<4;++row)if(hit(24,134+row*64,752,58,t)){
      lan::Computer c;if(lan::computer(computerOffset+row,c)){
        if(t.x>=670){strlcpy(optionsHost,c.id,33);editedOptions=lan::options(c.id);optionsFromMenu=false;page=Page::Options;}
        else page=lan::requestPlay(c.id)?Page::Pairing:Page::Result;
        dirty=true;
      }
    }
  } else if(page==Page::Pairing) {
    if(hit(656,24,120,52,t)){lan::cancelPlay();page=Page::Home;dirty=true;}
    else if(hit(24,364,200,58,t)){page=lan::requestPlay(lan::selectedId())?Page::Pairing:Page::Result;dirty=true;}
  } else if(page==Page::Result){
    if(hit(656,24,120,52,t)||hit(248,364,200,58,t)){lan::cancelPlay();page=Page::Home;dirty=true;}
    else if(hit(24,364,200,58,t)){page=lan::requestPlay(lan::selectedId())?Page::Pairing:Page::Result;dirty=true;}
  } else if(page==Page::Menu){
    if(hit(656,24,120,52,t)||hit(540,390,236,58,t)){pendingAction=Action::Resume;page=Page::Pairing;}
    else if(hit(24,390,224,58,t)){pendingAction=Action::Disconnect;page=Page::Home;}
    else if(hit(272,390,244,58,t)){pendingAction=Action::Reconnect;page=Page::Pairing;}
    else if(hit(24,122,176,58,t)){page=Page::Text;inputText[0]=0;symbols=uppercase=false;editingSsid=false;notice=(lan::options()&8)?"":"仅观看模式，请在电脑设置允许触控";}
    else if(hit(216,122,176,58,t)){uint32_t value=lan::options()^1;if(lan::saveOptions(lan::selectedId(),value)){actionNumber=value&1;pendingAction=Action::Mode;}}
    else if(hit(408,122,176,58,t)){strlcpy(optionsHost,lan::selectedId(),33);editedOptions=lan::options();optionsFromMenu=true;page=Page::Options;notice="";}
    else if(hit(600,122,176,58,t)){uint32_t value=lan::options()^2;if(lan::saveOptions(lan::selectedId(),value))pendingAction=Action::Stats;}
    else for(int i=0;i<8;++i)if((lan::options()&8)&&hit(24+(i%4)*192,222+(i/4)*66,176,54,t)){actionNumber=i;pendingAction=Action::Key;}
    dirty=true;
  } else if(page==Page::Options){
    if(hit(656,24,120,52,t)){page=optionsFromMenu?Page::Menu:Page::Home;}
    else if(hit(24,130,360,54,t))editedOptions^=1;
    else if(hit(416,130,360,54,t)){uint32_t monitor=((editedOptions>>8)&15)+1,maxMonitors=1;for(int i=0;i<lan::computerCount();++i){lan::Computer c;if(lan::computer(i,c)&&!strcmp(c.id,optionsHost))maxMonitors=c.monitors;}if(monitor>maxMonitors)monitor=1;editedOptions=(editedOptions&~(15UL<<8))|(monitor<<8);}
    else if(hit(24,194,360,54,t))editedOptions^=16;
    else if(hit(416,194,360,54,t))editedOptions^=4;
    else if(hit(24,258,360,54,t))editedOptions^=8;
    else if(hit(416,258,360,54,t)){uint32_t quality=((editedOptions>>16)&255)+20;if(quality>80)quality=40;editedOptions=(editedOptions&~(255UL<<16))|(quality<<16);}
    else if(hit(24,322,360,54,t))editedOptions^=2;
    else if(hit(416,322,360,54,t)){uint32_t fps=((editedOptions>>24)&63)+5;if(fps>30)fps=10;editedOptions=(editedOptions&~(63UL<<24))|(fps<<24);}
    else if(hit(24,406,360,54,t)){if(lan::saveOptions(optionsHost,editedOptions)){if(optionsFromMenu){pendingAction=Action::Reconnect;page=Page::Pairing;}else page=Page::Home;}else notice="设置保存失败，请重试";}
    dirty=true;
  } else if(page==Page::Forget){
    if(hit(408,390,368,58,t)){page=Page::Paired;notice="";}
    else if(hit(24,390,360,58,t)){
      if(pairing::forget(forgetHost)){lan::cancelPlay();lan::scanComputers();page=Page::Paired;notice="授权已删除，下次连接需要 PIN";}
      else notice=pairing::status();
      Serial.printf("Pairing delete: %s\n",pairing::paired(forgetHost)?"failed":"removed");
    }
    dirty=true;
  } else if(page==Page::Paired) {
    if(hit(656,24,120,52,t)){page=Page::Home;deleteIndex=-1;dirty=true;}
    else for(int i=0;i<pairing::count();++i)if(hit(642,136+i*76,120,48,t)){
      strlcpy(forgetHost,pairing::peerId(i),sizeof(forgetHost));strlcpy(forgetName,pairing::peerName(i),sizeof(forgetName));page=Page::Forget;notice="";
      dirty=true;break;
    }
  } else if (page==Page::Networks) {
    if (hit(656,24,120,52,t)) home();
    else if (hit(24,402,166,54,t) && !scanning) { requestScan=true;dirty=true; }
    else if (hit(202,402,166,54,t)) {
      cancelScan();selected[0]=password[0]=0;editingSsid=true;openNetwork=true;page=Page::Keyboard;notice="";dirty=true;
    } else if (hit(460,402,112,54,t)) { offset=max(0,offset-4);dirty=true; }
    else if (hit(584,402,112,54,t)) { if (offset+4<count) offset+=4;dirty=true; }
    else if (!scanning) for (int i=0;i<4 && offset+i<count;++i) if (hit(24,145+i*56,752,50,t)) {
      const auto &n=networks[offset+i];
      if (n.auth==WIFI_AUTH_WPA2_ENTERPRISE) { notice="暂不支持企业认证网络";dirty=true;break; }
      strlcpy(selected,n.ssid,sizeof(selected));memset(password,0,sizeof(password));
      openNetwork=n.auth==WIFI_AUTH_OPEN;editingSsid=false;showPassword=false;symbols=uppercase=false;
      page=Page::Keyboard;notice=openNetwork?"开放网络，可直接点连接":"请输入密码，连接成功后自动保存";dirty=true;break;
    }
  } else if (page==Page::Keyboard||page==Page::Text) {
    if(page==Page::Text&&hit(644,406,132,54,t)){page=Page::Menu;dirty=true;return;}
    if(page==Page::Text&&hit(496,406,138,54,t)){if(!(lan::options()&8))notice="仅观看模式，未发送文字";else if(!inputText[0])notice="请先输入文字";else{pendingAction=Action::Text;notice="已提交文字";}dirty=true;return;}
    if (page==Page::Keyboard&&!editingSsid && hit(648,105,126,40,t)) showPassword=!showPassword;
    else if (hit(24,406,108,54,t)) uppercase=!uppercase;
    else if (hit(142,406,108,54,t)) symbols=!symbols;
    else if (hit(644,406,132,54,t)) { memset(password,0,sizeof(password));page=Page::Networks;notice="轻触网络名称以连接"; }
    else if (hit(496,406,138,54,t)) {
      if (editingSsid) { if (selected[0]) { editingSsid=false;notice="输入密码；开放网络可留空"; } }
      else connect();
    } else {
      char *value=page==Page::Text?inputText:editingSsid?selected:password;const size_t capacity=page==Page::Text?sizeof(inputText):editingSsid?sizeof(selected):sizeof(password);size_t size=strlen(value);
      if (hit(378,406,108,54,t)) { if (size) value[size-1]=0; }
      else {
        char chosen=hit(260,406,108,54,t)?' ':0;
        for (int row=0;row<4;++row) {
          const char *k=keys(row);const int n=strlen(k),left=(800-n*74)/2;
          for (int col=0;col<n;++col) if (hit(left+col*74,164+row*54,66,46,t)) chosen=k[col];
        }
        if (chosen && size+1<capacity) { value[size]=chosen;value[size+1]=0;notice=""; }
      }
    }
    dirty=true;
  } else {
    if (hit(24,402,200,54,t)) home();
    else if (hit(240,402,200,54,t) && lan::joinState()==lan::JoinState::Failed) {
      page=Page::Keyboard;editingSsid=false;notice="请重新输入密码";dirty=true;
    }
  }
}
}
