#include "pairing.h"
#include <WiFi.h>
#include <Preferences.h>
#include <nvs.h>
#include "storage.h"
#include <esp_random.h>
#include <mbedtls/bignum.h>
#include <mbedtls/md.h>

namespace pairing {
namespace {
// RFC5054 Appendix A, 2048-bit group, generator 2. SHA256 throughout.
const char Prime[]=
 "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050"
 "A37329CBB4A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50"
 "E8083969EDB767B0CF6095179A163AB3661A05FBD5FAAAE82918A9962F0B93B8"
 "55F97993EC975EEAA80D740ADBF4FF747359D041D5C33EA71D281E446B14773B"
 "CA97B43A23FB801676BD207A436C6481F1D2B9078717461A5B9D32E688F87748"
 "544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB3786160279004E57AE6"
 "AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DBFBB6"
 "94B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";
struct Int:mbedtls_mpi { Int(){mbedtls_mpi_init(this);} ~Int(){mbedtls_mpi_free(this);} };
struct Record { char host[33];char key[33];char name[49]; };
struct Table { uint32_t version=2;Record records[4]={}; } stored;
char device[13]={},code[5]={},pendingHost[33]={};
char targetHost[33]={},targetName[49]={};
int find(const char *host) {
  for(int i=0;i<4;++i)if(stored.records[i].host[0]&&host&&!strcmp(stored.records[i].host,host))return i;
  return -1;
}
const char *message="尚未配对";
char storageError[112]={};
uint32_t opened=0;
unsigned attempts=0;
bool window=false,srpPending=false,authPending=false;
uint8_t salt[16]={},privateB[32]={},publicB[256]={},nonce[32]={};
bool validHex(const uint8_t *p,size_t n) {
  for(size_t i=0;i<n;++i) if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f'))) return false;
  return true;
}
bool equal(const uint8_t *a,const uint8_t *b,size_t n) {
  uint8_t d=0;for(size_t i=0;i<n;++i)d|=a[i]^b[i];return d==0;
}
void hex(const uint8_t *p,size_t n,char *out) {
  const char digits[]="0123456789abcdef";
  for(size_t i=0;i<n;++i){out[2*i]=digits[p[i]>>4];out[2*i+1]=digits[p[i]&15];}out[2*n]=0;
}
bool hash(const void *data,size_t n,uint8_t out[32]) {
  const auto *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md && !mbedtls_md(md,static_cast<const uint8_t *>(data),n,out);
}
bool mac(const uint8_t *key,size_t n,const void *data,size_t size,uint8_t out[32]) {
  const auto *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md && !mbedtls_md_hmac(md,key,n,static_cast<const uint8_t *>(data),size,out);
}
#define OK(x) do { if((x)!=0) return false; } while(0)
bool verifier(Int &N,Int &g,Int &v,Int &k) {
  OK(mbedtls_mpi_read_string(&N,16,Prime));OK(mbedtls_mpi_lset(&g,2));
  uint8_t bytes[512]={},digest[32];
  OK(mbedtls_mpi_write_binary(&N,bytes,256));bytes[511]=2;
  if(!hash(bytes,512,digest))return false;
  OK(mbedtls_mpi_read_binary(&k,digest,32));
  char identity[64];const int n=snprintf(identity,sizeof(identity),"MoonlightS3:%s:%s",device,code);
  if(!hash(identity,n,digest))return false;
  memcpy(bytes,salt,16);memcpy(bytes+16,digest,32);
  if(!hash(bytes,48,digest))return false;
  Int x;OK(mbedtls_mpi_read_binary(&x,digest,32));
  OK(mbedtls_mpi_exp_mod(&v,&g,&x,&N,nullptr));return true;
}
bool save(const Table &next) {
  nvs_handle_t handle=0;esp_err_t err=nvs_open_from_partition(storage::Partition,"ms3pair",NVS_READWRITE,&handle);
  if(err==ESP_OK)err=nvs_set_blob(handle,"peers",&next,sizeof(next));
  if(err==ESP_OK)err=nvs_commit(handle);
  if(err==ESP_OK){Table check;size_t length=sizeof(check);err=nvs_get_blob(handle,"peers",&check,&length);if(err==ESP_OK&&(length!=sizeof(next)||memcmp(&check,&next,sizeof(next))))err=ESP_FAIL;}
  if(handle)nvs_close(handle);
  if(err!=ESP_OK){nvs_stats_t stats={};nvs_get_stats(storage::Partition,&stats);Serial.printf("Pairing storage: %s used=%u free=%u total=%u\n",esp_err_to_name(err),stats.used_entries,stats.free_entries,stats.total_entries);snprintf(storageError,sizeof(storageError),"保存失败：%s",esp_err_to_name(err));message=storageError;}
  return err==ESP_OK;
}
}
void begin() {
  String id=WiFi.macAddress();id.replace(":","");id.toLowerCase();strlcpy(device,id.c_str(),sizeof(device));
  Preferences p;
  if(p.begin("ms3pair",true,storage::Partition)) {
    Table t;
    if(p.getBytesLength("peers")==sizeof(t)&&p.getBytes("peers",&t,sizeof(t))==sizeof(t)&&t.version==2) {
      for(auto &r:t.records)if(r.host[0]&&(r.host[32]||r.key[32]||r.name[48]||!validHex((uint8_t *)r.host,32)||!validHex((uint8_t *)r.key,32)))memset(&r,0,sizeof(r));
      stored=t;
    } else {
      struct Legacy {char host[33];char key[33];} r={};
      if(p.getBytesLength("peer")==sizeof(r)&&p.getBytes("peer",&r,sizeof(r))==sizeof(r)&&!r.host[32]&&!r.key[32]&&validHex((uint8_t *)r.host,32)&&validHex((uint8_t *)r.key,32)) {
        strlcpy(stored.records[0].host,r.host,33);strlcpy(stored.records[0].key,r.key,33);strlcpy(stored.records[0].name,"已配对电脑",49);
      }
    }
    p.end();
  }
  message=paired()?"已配对，可自动连接":"尚未配对";
}
bool paired(const char *host){return host?find(host)>=0:count()>0;}
int count(){int n=0;for(const auto &r:stored.records)if(r.host[0])++n;return n;}
const char *peerId(int index){for(const auto &r:stored.records)if(r.host[0]&&index--==0)return r.host;return "";}
const char *peerName(int index){for(const auto &r:stored.records)if(r.host[0]&&index--==0)return r.name[0]?r.name:r.host;return "";}
void abort(){srpPending=authPending=false;memset(privateB,0,sizeof(privateB));memset(nonce,0,sizeof(nonce));memset(pendingHost,0,sizeof(pendingHost));}
void cancel(){window=false;memset(code,0,sizeof(code));memset(targetHost,0,sizeof(targetHost));abort();}
void open(const char *host,const char *name){cancel();strlcpy(targetHost,host,sizeof(targetHost));strlcpy(targetName,name,sizeof(targetName));if(count()>=4&&!paired(host)){message="配对已满，请先在已配对设备中删除电脑";return;}uint32_t r;do{r=esp_random();}while(r>=4294960000UL);snprintf(code,sizeof(code),"%04lu",r%10000);opened=millis();attempts=0;window=true;message="请在电脑弹窗输入下方 PIN";}
bool enabled(){return window && millis()-opened<120000 && attempts<5;}
void poll(){if(window && (millis()-opened>=120000 || (attempts>=5&&!srpPending))){cancel();message="配对已关闭，请在屏上重新开启";}}
const char *pin(){return enabled()?code:"----";}
const char *status(){return message;}
bool forget(const char *host){int index=find(host);if(index<0){message="此电脑授权已不存在";return false;}Table next=stored;memset(&next.records[index],0,sizeof(Record));if(!save(next))return false;stored=next;cancel();message="已删除电脑授权";return true;}
bool start(const uint8_t *host,uint8_t out[288]) {
  abort();if(!enabled()||!validHex(host,32)||memcmp(host,targetHost,32))return false;
  ++attempts; // Includes interrupted attempts; reopening requires a physical screen action.
  memcpy(pendingHost,host,32);esp_fill_random(salt,16);esp_fill_random(privateB,32);privateB[0]|=0x80;
  Int N,g,v,k,b,B,temp;
  if(!verifier(N,g,v,k))return false;
  OK(mbedtls_mpi_read_binary(&b,privateB,32));OK(mbedtls_mpi_exp_mod(&B,&g,&b,&N,nullptr));
  OK(mbedtls_mpi_mul_mpi(&temp,&k,&v));OK(mbedtls_mpi_add_mpi(&temp,&temp,&B));
  OK(mbedtls_mpi_mod_mpi(&B,&temp,&N));OK(mbedtls_mpi_write_binary(&B,publicB,256));
  memcpy(out,"SRP2",4);memcpy(out+4,device,12);memcpy(out+16,salt,16);memcpy(out+32,publicB,256);
  srpPending=true;return true;
}
bool finish(const uint8_t request[292],uint8_t reply[48]) {
  // The fifth attempt may finish, but no sixth attempt can start.
  if(!srpPending||!window||millis()-opened>=120000||memcmp(request,"SRA2",4))return false;
  srpPending=false;
  Int N,g,v,k,A,b,u,temp,S;
  if(!verifier(N,g,v,k))return false;
  OK(mbedtls_mpi_read_binary(&A,request+4,256));
  if(mbedtls_mpi_cmp_int(&A,1)<=0 || mbedtls_mpi_cmp_mpi(&A,&N)>=0)return false;
  uint8_t bytes[512],digest[32],key[32];memcpy(bytes,request+4,256);memcpy(bytes+256,publicB,256);
  if(!hash(bytes,512,digest))return false;
  OK(mbedtls_mpi_read_binary(&u,digest,32));if(!mbedtls_mpi_cmp_int(&u,0))return false;
  OK(mbedtls_mpi_read_binary(&b,privateB,32));OK(mbedtls_mpi_exp_mod(&temp,&v,&u,&N,nullptr));
  OK(mbedtls_mpi_mul_mpi(&temp,&temp,&A));OK(mbedtls_mpi_mod_mpi(&temp,&temp,&N));
  OK(mbedtls_mpi_exp_mod(&S,&temp,&b,&N,nullptr));OK(mbedtls_mpi_write_binary(&S,bytes,256));
  if(!hash(bytes,256,key))return false;
  uint8_t transcript[582]; // label10, device12, host32, salt16, A256, B256
  memcpy(transcript+10,device,12);memcpy(transcript+22,pendingHost,32);memcpy(transcript+54,salt,16);
  memcpy(transcript+70,request+4,256);memcpy(transcript+326,publicB,256);
  memcpy(transcript,"client-v2:",10);
  if(!mac(key,32,transcript,sizeof(transcript),digest)||!equal(digest,request+260,32)) {
    message="验证码不正确，请重试";memset(key,0,sizeof(key));return false;
  }
  Record next={};strlcpy(next.host,pendingHost,sizeof(next.host));
  memcpy(transcript,"secret-v2:",10);
  if(!mac(key,32,transcript,sizeof(transcript),digest))return false;
  hex(digest,16,next.key);
  memcpy(transcript,"server-v2:",10);
  if(!mac(key,32,transcript,sizeof(transcript),digest))return false;
  memset(key,0,sizeof(key));
  strlcpy(next.name,targetName,sizeof(next.name));Table table=stored;int slot=find(next.host);
  if(slot<0)for(int i=0;i<4;++i)if(!table.records[i].host[0]){slot=i;break;}
  if(slot<0)return false;table.records[slot]=next;
  if(millis()-opened>=120000||!save(table)){message="保存配对失败，请重试";return false;}
  stored=table;memcpy(reply,"PAK2",4);memcpy(reply+4,digest,32);memcpy(reply+36,device,12);
  cancel();message="配对成功，后续自动验证";return true;
}
bool challenge(const uint8_t *host,uint8_t out[48]) {
  abort();char id[33]={};memcpy(id,host,32);if(!validHex(host,32)||!paired(id))return false;
  memcpy(pendingHost,host,32);
  esp_fill_random(nonce,32);memcpy(out,"CHL2",4);memcpy(out+4,device,12);memcpy(out+16,nonce,32);
  authPending=true;return true;
}
bool authenticate(const uint8_t proof[68],uint8_t out[68]) {
  if(!authPending)return false;authPending=false;
  const int index=find(pendingHost);if(index<0)return false;const auto &record=stored.records[index];
  uint8_t data[82],digest[32];char expected[65];
  memcpy(data,"client",6);memcpy(data+6,device,12);memcpy(data+18,record.host,32);memcpy(data+50,nonce,32);
  if(!mac((uint8_t *)record.key,32,data,sizeof(data),digest))return false;hex(digest,32,expected);
  if(memcmp(proof,"PRF2",4)||!equal(proof+4,(uint8_t *)expected,64))return false;
  memcpy(data,"server",6);if(!mac((uint8_t *)record.key,32,data,sizeof(data),digest))return false;
  memcpy(out,"OKA2",4);hex(digest,32,expected);memcpy(out+4,expected,64);return true;
}
void signature(const char *host,const char *data,size_t length,char out[65]) {
  uint8_t digest[32];out[0]=0;
  const int index=find(host);
  if(index>=0&&mac((uint8_t *)stored.records[index].key,32,data,length,digest))hex(digest,32,out);
}
}
