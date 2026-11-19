#include <esp_partition.h>
#include <algorithm>
#include <cstring>
#include <vector>
bool host_partition_present=false,host_partition_fail_read=false,host_partition_fail_write=false;
esp_partition_t host_partition{ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_TEST,0x7f0000,0x100000,"logs"};
size_t host_partition_reads=0,host_partition_writes=0,host_partition_erases=0;
namespace {std::vector<uint8_t> bytes(0x100000,0xff);}
const esp_partition_t* esp_partition_find_first(esp_partition_type_t type,esp_partition_subtype_t subtype,const char* label){
  return host_partition_present&&type==host_partition.type&&subtype==host_partition.subtype&&std::strcmp(label,host_partition.label)==0?&host_partition:nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t*,size_t offset,void* out,size_t size){
  ++host_partition_reads;if(host_partition_fail_read||offset>bytes.size()||size>bytes.size()-offset)return ESP_FAIL;
  std::memcpy(out,bytes.data()+offset,size);return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t*,size_t offset,const void* source,size_t size){
  ++host_partition_writes;if(host_partition_fail_write||offset>bytes.size()||size>bytes.size()-offset)return ESP_FAIL;
  const auto* input=static_cast<const uint8_t*>(source);
  for(size_t i=0;i<size;++i)if((bytes[offset+i]&input[i])!=input[i])return ESP_FAIL;
  for(size_t i=0;i<size;++i)bytes[offset+i]&=input[i];
  return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t*,size_t offset,size_t size){
  ++host_partition_erases;if(host_partition_fail_write||offset%4096||size!=4096||offset>bytes.size()||size>bytes.size()-offset)return ESP_FAIL;
  std::fill(bytes.begin()+offset,bytes.begin()+offset+size,0xff);return ESP_OK;
}
