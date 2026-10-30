#include "storage.h"
#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <memory>

namespace storage {
namespace {
bool migrate(const char *space,const char *key){
  nvs_handle_t target=0,source=0;
  esp_err_t err=nvs_open_from_partition(Partition,space,NVS_READWRITE,&target);
  if(err!=ESP_OK)return false;
  size_t length=0;
  err=nvs_get_blob(target,key,nullptr,&length);
  if(err==ESP_OK){nvs_close(target);return true;}
  if(err!=ESP_ERR_NVS_NOT_FOUND){nvs_close(target);return false;}
  err=nvs_open(space,NVS_READONLY,&source);
  if(err==ESP_ERR_NVS_NOT_FOUND){nvs_close(target);return true;}
  if(err==ESP_OK)err=nvs_get_blob(source,key,nullptr,&length);
  if(err==ESP_ERR_NVS_NOT_FOUND){if(source)nvs_close(source);nvs_close(target);return true;}
  std::unique_ptr<uint8_t[]> data;
  if(err==ESP_OK&&length&&length<=4096)data.reset(new(std::nothrow) uint8_t[length]);
  if(!data)err=ESP_ERR_NO_MEM;
  if(err==ESP_OK)err=nvs_get_blob(source,key,data.get(),&length);
  if(err==ESP_OK)err=nvs_set_blob(target,key,data.get(),length);
  if(err==ESP_OK)err=nvs_commit(target);
  if(source)nvs_close(source);nvs_close(target);
  Serial.printf("Store migration %s/%s: %s\n",space,key,esp_err_to_name(err));
  return err==ESP_OK;
}
}
bool begin(){
  const esp_err_t err=nvs_flash_init_partition(Partition);
  if(err!=ESP_OK){Serial.printf("Dedicated store init: %s; data preserved\n",esp_err_to_name(err));return false;}
  // Missing keys migrate once. Existing keys, including an empty peers table, win.
  if(!migrate("moonlight","wifi")||!migrate("ms3pair","peers")||!migrate("ms3pair","peer")||!migrate("ms3ui","profiles"))return false;
  nvs_stats_t stats={};nvs_get_stats(Partition,&stats);
  Serial.printf("Dedicated store ready: used=%u free=%u total=%u\n",stats.used_entries,stats.free_entries,stats.total_entries);
  return true;
}
}
