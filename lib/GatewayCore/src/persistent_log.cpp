#include "persistent_log.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace gateway {
namespace {
constexpr size_t kSectorSize=4096,kSlotSize=256,kSectorCount=256,kRecords=15;
constexpr size_t kCrcOffset=252,kMessageOffset=32,kMessageLimit=180;
constexpr uint32_t kMagic=0x474f4c50; // "PLOG", little endian
constexpr uint16_t kVersion=1;
using Block=std::array<uint8_t,kSlotSize>;

uint16_t get16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint32_t get32(const uint8_t* p){return uint32_t(get16(p))|(uint32_t(get16(p+2))<<16);}
uint64_t get64(const uint8_t* p){return uint64_t(get32(p))|(uint64_t(get32(p+4))<<32);}
void put16(uint8_t* p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
void put32(uint8_t* p,uint32_t v){put16(p,uint16_t(v));put16(p+2,uint16_t(v>>16));}
void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
uint32_t crc32(const uint8_t* data,size_t length){
  uint32_t crc=0xffffffffu;
  for(size_t i=0;i<length;++i){crc^=data[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&uint32_t(0u-(crc&1u)));}
  return ~crc;
}
bool blank(const Block& b){return std::all_of(b.begin(),b.end(),[](uint8_t v){return v==0xff;});}
bool validCrc(const Block& b){return get32(b.data()+kCrcOffset)==crc32(b.data(),kCrcOffset);}
void finish(Block& b){put32(b.data()+kCrcOffset,crc32(b.data(),kCrcOffset));}
bool initialHeaderPrefix(const Block& block){
  Block expected{};put32(expected.data(),kMagic);put16(expected.data()+4,kVersion);
  put64(expected.data()+8,1);put64(expected.data()+16,1);finish(expected);
  for(size_t cut=1;cut<block.size();++cut){
    if(std::equal(block.begin(),block.begin()+cut,expected.begin())&&
       std::all_of(block.begin()+cut,block.end(),[](uint8_t value){return value==0xff;}))return true;
  }
  return false;
}
bool newer(uint32_t candidate,uint32_t reference){return candidate!=reference&&uint32_t(candidate-reference)<0x80000000u;}
bool validType(EventType type){const auto value=static_cast<int>(type);return value>=static_cast<int>(EventType::LinkLost)&&value<=static_cast<int>(EventType::SyslogTest);}
bool validEvent(const Event& e){return e.seq!=0&&validType(e.type)&&e.severity>=1&&e.severity<=3&&e.message.size()<=kMessageLimit;}
Block encodeEvent(const Event& e){
  Block b{};put32(b.data(),e.seq);put64(b.data()+4,e.uptime_ms);put64(b.data()+12,e.utc);
  put16(b.data()+20,static_cast<uint16_t>(e.type));b[22]=e.severity;b[23]=uint8_t(e.message.size());
  std::memcpy(b.data()+kMessageOffset,e.message.data(),e.message.size());finish(b);return b;
}
bool decodeEvent(const Block& b,Event& e){
  if(!validCrc(b))return false;
  const auto type=get16(b.data()+20);const size_t length=b[23];
  if(get32(b.data())==0||type>static_cast<unsigned>(EventType::SyslogTest)||b[22]<1||b[22]>3||length>kMessageLimit)return false;
  e.seq=get32(b.data());e.uptime_ms=get64(b.data()+4);e.utc=get64(b.data()+12);e.type=static_cast<EventType>(type);e.severity=b[22];
  e.message.assign(reinterpret_cast<const char*>(b.data()+kMessageOffset),length);return true;
}
}
PersistentLog::PersistentLog(PersistentLogStorage& storage):storage_(storage){}

bool PersistentLog::begin(){
  initialized_=false;healthy_=false;sectors_={};generation_=epoch_=0;current_=kSectorCount;count_=0;latest_sequence_=0;
  if(storage_.size()!=kSectorSize*kSectorCount||!scan())return false;
  initialized_=healthy_=true;return true;
}
bool PersistentLog::scan(){
  bool saw_data=false,initial_torn=false;
  for(size_t index=0;index<kSectorCount;++index){
    auto& sector=sectors_[index];Block header{};
    if(!storage_.read(index*kSectorSize,header.data(),header.size()))return false;
    const bool empty_header=blank(header);
    if(!empty_header){
      saw_data=true;if(index!=0)initial_torn=false;
      if(index==0&&initialHeaderPrefix(header))initial_torn=true;
      if(get32(header.data())==kMagic&&validCrc(header)&&get16(header.data()+4)!=kVersion)return false;
      if(get32(header.data())==kMagic&&get16(header.data()+4)==kVersion&&validCrc(header)){
        sector.generation=get64(header.data()+8);sector.epoch=get64(header.data()+16);sector.watermark=get32(header.data()+24);
        if(sector.generation==0||sector.epoch==0||sector.epoch>sector.generation)return false;
        sector.valid=true;
      }else sector.dirty=true;
    }
    for(size_t n=0;n<kRecords;++n){
      Block block{};if(!storage_.read(index*kSectorSize+(n+1)*kSlotSize,block.data(),block.size()))return false;
      if(blank(block))continue;
      saw_data=true;if(current_==kSectorCount)initial_torn=false;
      if(!sector.valid){sector.dirty=true;continue;}
      if(sector.dirty||n!=sector.count){sector.dirty=true;continue;}
      Event event;
      if(!decodeEvent(block,event)||(sector.count? !newer(event.seq,sector.last):(sector.watermark!=0&&!newer(event.seq,sector.watermark)))){sector.dirty=true;continue;}
      if(!sector.count)sector.first=event.seq;sector.last=event.seq;++sector.count;
    }
    if(sector.valid){
      if(current_!=kSectorCount&&sector.generation==generation_)return false;
      if(current_==kSectorCount||sector.generation>generation_){generation_=sector.generation;epoch_=sector.epoch;current_=index;}
    }
  }
  if(current_==kSectorCount){
    if(initial_torn){current_=0;return true;}
    return !saw_data;
  }
  // Ring generations occupy their matching physical slots. This also rejects
  // unrelated but coincidentally CRC-valid sectors before reusing their bytes.
  for(size_t i=0;i<kSectorCount;++i)if(sectors_[i].valid){
    const auto& sector=sectors_[i];
    if((generation_-sector.generation)%kSectorCount!=(current_+kSectorCount-i)%kSectorCount||
       generation_-sector.generation>=kSectorCount||sector.epoch>epoch_)return false;
  }
  const auto& current=sectors_[current_];latest_sequence_=current.count?current.last:current.watermark;
  for(const auto& sector:sectors_)if(sector.valid&&sector.epoch==epoch_&&generation_-sector.generation<kSectorCount-1)count_+=sector.count;
  return true;
}
bool PersistentLog::rotate(bool clearing,uint32_t watermark){
  if(generation_==std::numeric_limits<uint64_t>::max())return false;
  const size_t next=current_==kSectorCount?0:(current_+1)%kSectorCount;
  if(sectors_[next].valid||sectors_[next].dirty){
    if(!storage_.erase(next*kSectorSize,kSectorSize))return false;
  }
  const uint64_t generation=generation_+1;
  const uint64_t epoch=clearing||epoch_==0?generation:epoch_;
  Block header{};put32(header.data(),kMagic);put16(header.data()+4,kVersion);put64(header.data()+8,generation);
  put64(header.data()+16,epoch);put32(header.data()+24,watermark);finish(header);
  if(!storage_.write(next*kSectorSize,header.data(),header.size()))return false;
  if(clearing)count_=0;
  else {
    const auto& retired=sectors_[(next+1)%kSectorCount];
    if(retired.valid&&retired.epoch==epoch_&&generation-retired.generation==kSectorCount-1)count_-=retired.count;
  }
  sectors_[next]={};auto& sector=sectors_[next];sector.valid=true;sector.generation=generation;sector.epoch=epoch;sector.watermark=watermark;
  current_=next;generation_=generation;epoch_=epoch;latest_sequence_=watermark;return true;
}
bool PersistentLog::append(const std::vector<Event>& events){
  if(!initialized_||!healthy_)return false;
  uint32_t prior=latest_sequence_;
  for(const auto& event:events){if(!validEvent(event)||(prior&&!newer(event.seq,prior)))return false;prior=event.seq;}
  for(const auto& event:events){
    if(current_==kSectorCount||sectors_[current_].dirty||sectors_[current_].count==kRecords){
      if(!rotate(false,latest_sequence_)){healthy_=false;return false;}
    }
    auto& sector=sectors_[current_];const Block block=encodeEvent(event);
    if(!storage_.write(current_*kSectorSize+(size_t(sector.count)+1)*kSlotSize,block.data(),block.size())){sector.dirty=true;healthy_=false;return false;}
    if(!sector.count)sector.first=event.seq;sector.last=event.seq;++sector.count;++count_;latest_sequence_=event.seq;
  }
  return true;
}
bool PersistentLog::clear(uint32_t sequence){
  if(!initialized_||!healthy_)return false;
  if(latest_sequence_&&(sequence==0||(sequence!=latest_sequence_&&!newer(sequence,latest_sequence_))))return false;
  if(!rotate(true,sequence)){healthy_=false;return false;}return true;
}
bool PersistentLog::readRecord(size_t sector,size_t index,Event& event){
  Block block{};return storage_.read(sector*kSectorSize+(index+1)*kSlotSize,block.data(),block.size())&&decodeEvent(block,event);
}
bool PersistentLog::recent(std::vector<Event>& out,size_t limit,bool has_before,uint32_t before){
  out.clear();if(!initialized_)return false;
  limit=std::min<size_t>(limit,48);if(!limit||current_==kSectorCount)return true;
  out.reserve(std::min(limit,count_));
  for(size_t distance=0;distance<kSectorCount-1&&out.size()<limit;++distance){
    const size_t index=(current_+kSectorCount-distance)%kSectorCount;const auto& sector=sectors_[index];
    if(!sector.valid||sector.epoch!=epoch_||!sector.count)continue;
    if(has_before&&!newer(before,sector.first))continue;
    for(size_t n=sector.count;n>0&&out.size()<limit;--n){
      Event event;if(!readRecord(index,n-1,event)){out.clear();healthy_=false;return false;}
      if(!has_before||newer(before,event.seq))out.push_back(std::move(event));
    }
  }
  return true;
}
}
