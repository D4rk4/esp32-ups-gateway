#include "persistent_log_store.hpp"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace gateway {
namespace {
constexpr size_t kLogOffset=0x7f0000,kLogSize=0x100000,kSectorSize=4096;
}
bool PersistentLogStore::begin(std::string& error){
  partition_=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,static_cast<esp_partition_subtype_t>(0x40),"logs");
  if(!partition_||partition_->address!=kLogOffset||partition_->size!=kLogSize){
    partition_=nullptr;error="Persistent log partition is missing or has an unexpected layout";return false;
  }
  error.clear();return true;
}
size_t PersistentLogStore::size()const{return partition_?partition_->size:0;}
bool PersistentLogStore::read(size_t offset,void* destination,size_t length){
  if(!partition_||offset>size()||length>size()-offset)return false;
  const bool ok=esp_partition_read(partition_,offset,destination,length)==ESP_OK;
  if(offset%kSectorSize==0)vTaskDelay(1);
  return ok;
}
bool PersistentLogStore::write(size_t offset,const void* source,size_t length){
  return partition_&&offset<=size()&&length<=size()-offset&&esp_partition_write(partition_,offset,source,length)==ESP_OK;
}
bool PersistentLogStore::erase(size_t offset,size_t length){
  return partition_&&length==kSectorSize&&offset%kSectorSize==0&&offset<=size()&&length<=size()-offset&&esp_partition_erase_range(partition_,offset,length)==ESP_OK;
}
}
