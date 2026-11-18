#include "persistent_log.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace gateway;
namespace {
constexpr size_t sector=4096, sectors=256;
struct Memory final : PersistentLogStorage {
  std::vector<uint8_t> bytes;
  std::vector<unsigned> erases;
  size_t reads=0,read_bytes=0,writes=0,erase_count=0;
  long fail_read=-1,fail_write=-1,fail_erase=-1;
  size_t partial=0;
  explicit Memory(size_t size=sector*sectors):bytes(size,0xff),erases((size+sector-1)/sector){}
  size_t size() const override {return bytes.size();}
  bool read(size_t offset,void* out,size_t length) override {
    ++reads;read_bytes+=length;if(fail_read==0)return false;if(fail_read>0)--fail_read;
    if(offset>bytes.size()||length>bytes.size()-offset)return false;
    std::memcpy(out,bytes.data()+offset,length);return true;
  }
  bool write(size_t offset,const void* data,size_t length) override {
    ++writes;if(offset>bytes.size()||length>bytes.size()-offset)return false;
    const bool fail=fail_write==0;if(fail_write>0)--fail_write;
    const size_t amount=fail?std::min(length,partial):length;
    const auto* input=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<amount;++i){if((bytes[offset+i]&input[i])!=input[i])return false;bytes[offset+i]&=input[i];}
    return !fail;
  }
  bool erase(size_t offset,size_t length) override {
    ++erase_count;if(fail_erase==0)return false;if(fail_erase>0)--fail_erase;
    if(offset%sector||length!=sector||offset>bytes.size()||length>bytes.size()-offset)return false;
    std::fill(bytes.begin()+offset,bytes.begin()+offset+length,0xff);++erases[offset/sector];return true;
  }
};
uint32_t headerCrc(const uint8_t* data,size_t length){
  uint32_t crc=0xffffffffu;
  for(size_t i=0;i<length;++i){crc^=data[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&uint32_t(0u-(crc&1u)));}
  return ~crc;
}
void setHeaderCrc(std::vector<uint8_t>& bytes,size_t offset){
  const uint32_t crc=headerCrc(bytes.data()+offset,252);
  for(size_t i=0;i<4;++i)bytes[offset+252+i]=uint8_t(crc>>(i*8));
}
Event event(uint32_t seq,const std::string& message="event"){
  return {seq,uint64_t(seq)*1000,EventType::Startup,1,message,uint64_t(seq)+1700000000};
}
std::vector<Event> page(PersistentLog& log,size_t limit=48,bool has_before=false,uint32_t before=0){
  std::vector<Event> out;assert(log.recent(out,limit,has_before,before));return out;
}
void same(const Event& a,const Event& b){assert(a.seq==b.seq&&a.uptime_ms==b.uptime_ms&&a.utc==b.utc&&a.type==b.type&&a.severity==b.severity&&a.message==b.message);}
void empty_and_persistence(){
  Memory mem;PersistentLog log(mem);assert(log.begin());assert(log.healthy()&&log.capacity()==3825&&log.count()==0&&log.latestSequence()==0);
  assert(page(log).empty());assert(mem.erase_count==0&&mem.writes==0);
  std::string long_text(180,'x');const std::vector<Event> events{event(1),event(2,long_text),event(3,"third")};
  assert(log.append(events));assert(log.count()==3&&log.latestSequence()==3);assert(mem.erase_count==0);
  auto got=page(log);assert(got.size()==3);for(size_t i=0;i<3;++i)same(got[i],events[2-i]);
  got=page(log,1,true,3);assert(got.size()==1&&got[0].seq==2);
  PersistentLog reboot(mem);assert(reboot.begin());assert(reboot.count()==3&&reboot.latestSequence()==3);got=page(reboot);assert(got.size()==3);for(size_t i=0;i<3;++i)same(got[i],events[2-i]);
  assert(mem.erase_count==0);
}
void rollover_and_rotation(){
  Memory mem;PersistentLog log(mem);assert(log.begin());
  for(uint32_t seq=1;seq<=3840+sector*2;seq+=15){std::vector<Event> batch;for(uint32_t n=seq;n<seq+15&&n<=3840+sector*2;++n)batch.push_back(event(n));assert(log.append(batch));}
  assert(log.count()<=3825&&log.count()>=3811);auto got=page(log);assert(got.size()==48&&got.front().seq==3840+sector*2);
  auto minmax=std::minmax_element(mem.erases.begin(),mem.erases.end());assert(*minmax.second-*minmax.first<=1);assert(mem.erase_count>0);
  PersistentLog reboot(mem);assert(reboot.begin());assert(reboot.count()==log.count()&&reboot.latestSequence()==log.latestSequence());assert(page(reboot).front().seq==got.front().seq);
  Memory wrap;PersistentLog wrapped(wrap);assert(wrapped.begin());assert(wrapped.append({event(0xfffffffe),event(0xffffffff),event(1),event(2)}));
  got=page(wrapped,48,true,1);assert(got.size()==2&&got[0].seq==0xffffffff&&got[1].seq==0xfffffffe);
  PersistentLog again(wrap);assert(again.begin()&&again.latestSequence()==2);assert(page(again).front().seq==2);
}
void durable_clear(){
  Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({event(1),event(2)}));const auto before=mem.erase_count;
  assert(log.clear(7));assert(log.count()==0&&log.latestSequence()==7&&page(log).empty());assert(mem.erase_count<=before+1);
  PersistentLog reboot(mem);assert(reboot.begin()&&reboot.count()==0&&reboot.latestSequence()==7);assert(page(reboot).empty());
  assert(reboot.append({event(8)}));PersistentLog second(mem);assert(second.begin()&&second.count()==1&&second.latestSequence()==8);
  auto got=page(second);assert(got.size()==1&&got[0].seq==8);
}
void failed_full_clear_preserves_history(){
  for(size_t cut:{size_t(0),size_t(1),size_t(128),size_t(255)}){
    Memory mem;PersistentLog log(mem);assert(log.begin());
    for(uint32_t n=1;n<=3825;n+=15){std::vector<Event> batch;for(uint32_t i=n;i<n+15;++i)batch.push_back(event(i));assert(log.append(batch));}
    assert(log.count()==3825);mem.fail_write=0;mem.partial=cut;
    assert(!log.clear(3825)&&!log.healthy());mem.fail_write=-1;
    PersistentLog reboot(mem);assert(reboot.begin()&&reboot.count()==3825&&reboot.latestSequence()==3825);
    auto oldest=page(reboot,48,true,16);assert(oldest.size()==15&&oldest.back().seq==1);
    assert(reboot.append({event(3826)}));assert(reboot.count()==3811);
  }
}
void interrupted_recycled_sector_erase(){
  for(bool version_erased:{true,false}){
    Memory mem;PersistentLog log(mem);assert(log.begin());
    for(uint32_t n=1;n<=3840;n+=15){std::vector<Event> batch;for(uint32_t i=n;i<n+15;++i)batch.push_back(event(i));assert(log.append(batch));}
    assert(log.count()==3825);mem.fail_erase=0;
    // Model a failed erase that changes some zero bits to one, not a clean erase.
    if(version_erased)std::fill(mem.bytes.begin()+4,mem.bytes.begin()+8,0xff);
    else std::fill(mem.bytes.begin(),mem.bytes.begin()+4,0xff);
    assert(!log.append({event(3841)})&&!log.healthy());mem.fail_erase=-1;
    PersistentLog reboot(mem);assert(reboot.begin()&&reboot.count()==3825&&reboot.latestSequence()==3840);
    auto got=page(reboot,48,true,31);assert(got.size()==15&&got.back().seq==16);
    assert(reboot.append({event(3841)}));PersistentLog again(mem);assert(again.begin()&&again.latestSequence()==3841);
  }
}
void extended_query_type_bounds(){
  const auto last=EventType::ExtendedQuery;const auto past=static_cast<EventType>(static_cast<int>(last)+1);
  auto typed=[](uint32_t seq,EventType type){auto e=event(seq,"UPS extended query BT supported");e.type=type;return e;};
  {
    Memory mem;PersistentLog log(mem);assert(log.begin());
    // A batch with the newest type must persist whole; rejecting it would also drop its valid neighbours.
    assert(log.append({event(1),typed(2,last),event(3)}));assert(log.count()==3&&log.healthy());
    PersistentLog reboot(mem);assert(reboot.begin()&&reboot.count()==3&&reboot.latestSequence()==3);
    auto got=page(reboot);assert(got.size()==3);same(got[1],typed(2,last));assert(got[1].type==EventType::ExtendedQuery);
  }
  {
    Memory mem;PersistentLog log(mem);assert(log.begin());
    assert(!log.append({typed(1,past)})&&!log.append({event(1),typed(2,past)}));assert(log.healthy()&&mem.writes==0);
  }
  for(const bool in_range:{true,false}){
    Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({typed(1,last)}));
    const size_t record=256,type_offset=record+20;const auto raw=static_cast<uint16_t>(in_range?last:past);
    mem.bytes[type_offset]=uint8_t(raw);mem.bytes[type_offset+1]=uint8_t(raw>>8);setHeaderCrc(mem.bytes,record);
    PersistentLog reboot(mem);assert(reboot.begin());assert(reboot.count()==(in_range?1u:0u));
  }
}
Event typed(uint32_t seq,EventType type){auto e=event(seq,"typed");e.type=type;return e;}
void fill(PersistentLog& log,uint32_t from,uint32_t to,EventType type=EventType::Startup){
  for(uint32_t n=from;n<=to;n+=15){std::vector<Event> batch;for(uint32_t i=n;i<n+15&&i<=to;++i)batch.push_back(typed(i,type));assert(log.append(batch));}
}
void newest_of_type(){
  const auto battery=EventType::OnBattery;Event out=event(77);
  {
    Memory mem;PersistentLog log(mem);assert(log.begin());
    mem.reads=0;assert(!log.newest(battery,out)&&out.seq==77&&mem.reads==0);// empty log: none, out untouched, no reads
    assert(log.append({event(1),typed(2,battery),event(3),typed(4,EventType::PowerRestored),typed(9,battery),event(10)}));
    mem.reads=0;assert(log.newest(battery,out));same(out,typed(9,battery));assert(mem.reads==2);// stops at the first match, newest first
    PersistentLog reboot(mem);assert(reboot.begin());assert(reboot.newest(battery,out));same(out,typed(9,battery));
    assert(!reboot.newest(EventType::LowBattery,out)&&out.seq==9);// other types are ignored
  }
  {
    Memory mem;PersistentLog log(mem);assert(log.begin());
    assert(log.append({typed(1,battery)}));fill(log,2,400);// older than the newest 48 records and several sectors back
    assert(page(log).front().seq==400&&page(log).back().seq==353);
    assert(log.newest(battery,out)&&out.seq==1);
    PersistentLog reboot(mem);assert(reboot.begin());assert(reboot.newest(battery,out)&&out.seq==1);
    assert(log.clear(log.latestSequence()));assert(!log.newest(battery,out));
    PersistentLog cleared(mem);assert(cleared.begin());assert(!cleared.newest(battery,out));
    assert(cleared.append({event(401)}));assert(!cleared.newest(battery,out));
    assert(cleared.append({typed(402,battery)}));assert(cleared.newest(battery,out)&&out.seq==402);
  }
  {
    Memory mem;PersistentLog log(mem);assert(log.begin());
    assert(log.append({typed(1,battery)}));fill(log,2,3840+sector*2);// rotated out of the ring: gone, as for recent()
    assert(!log.newest(battery,out));
    PersistentLog reboot(mem);assert(reboot.begin());assert(!reboot.newest(battery,out));
  }
  {
    Memory mem;PersistentLog log(mem);assert(log.begin());fill(log,1,3825);assert(log.count()==3825);
    mem.reads=mem.read_bytes=0;assert(!log.newest(battery,out));// worst case: full log without a match reads every record once
    assert(mem.reads<=log.capacity()&&mem.read_bytes<=log.capacity()*256&&log.healthy());
  }
  {
    // Changed contract: a failed read in newest() is "not known", not a fault.
    Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({event(1),typed(2,battery)}));
    Event kept=event(55);mem.fail_read=0;assert(!log.newest(battery,kept)&&kept.seq==55&&log.healthy());// false, out untouched, log still healthy
    mem.fail_read=-1;assert(log.append({event(3)}));// journaling continues after a failed search
    PersistentLog reboot(mem);assert(reboot.begin()&&reboot.newest(battery,out)&&out.seq==2);
  }
  {
    // A failed read must not skip the unreadable record and present an older match.
    Memory mem;PersistentLog log(mem);assert(log.begin());
    assert(log.append({typed(1,battery),typed(2,battery)}));
    Event out2=event(77);mem.fail_read=0;assert(!log.newest(battery,out2)&&out2.seq==77&&log.healthy());
  }
}
void damaged_records_and_invalid_input(){
  {
    Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({event(1),event(2),event(3)}));
    mem.bytes[2*256+40]^=1;PersistentLog reboot(mem);assert(reboot.begin());
    assert(reboot.latestSequence()==1&&reboot.count()==1);assert(reboot.append({event(4)}));
    PersistentLog again(mem);assert(again.begin()&&again.count()==2&&again.latestSequence()==4);
  }
  {
    Memory mem;PersistentLog log(mem);assert(log.begin());
    assert(!log.append({event(0)})&&!log.append({event(1,std::string(181,'x'))}));
    auto bad=event(1);bad.type=static_cast<EventType>(999);assert(!log.append({bad}));
    bad=event(1);bad.severity=9;assert(!log.append({bad}));assert(log.healthy()&&mem.writes==0);
    assert(log.append({event(2)}));assert(!log.append({event(2)}));assert(!log.clear(1));assert(log.healthy());
  }
  {
    Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({event(1)}));
    mem.bytes[4]=2;setHeaderCrc(mem.bytes,0);PersistentLog foreign(mem);assert(!foreign.begin()&&!foreign.healthy());
  }
  for(size_t cut:{size_t(0),size_t(1),size_t(2),size_t(3),size_t(4),size_t(5),size_t(6),size_t(16),size_t(255)}){
    Memory mem;PersistentLog log(mem);assert(log.begin());mem.fail_write=0;mem.partial=cut;assert(!log.append({event(1)}));
    mem.fail_write=-1;PersistentLog reboot(mem);assert(reboot.begin()&&reboot.count()==0&&reboot.latestSequence()==0);
    assert(reboot.append({event(1)}));
  }
}
void errors_and_torn_writes(){
  for(size_t cut:{size_t(0),size_t(1),size_t(10),size_t(128),size_t(255)}){
    Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({event(1)}));mem.fail_write=0;mem.partial=cut;
    assert(!log.append({event(2)}));assert(!log.healthy());assert(!log.append({event(2)}));
    mem.fail_write=-1;PersistentLog reboot(mem);assert(reboot.begin());assert(reboot.latestSequence()==1&&page(reboot).front().seq==1);
    assert(reboot.append({event(2)}));PersistentLog again(mem);assert(again.begin()&&again.latestSequence()==2);
  }
  for(size_t cut:{size_t(0),size_t(1),size_t(32),size_t(255)}){
    Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({event(1)}));mem.fail_write=0;mem.partial=cut;
    assert(!log.clear(1)&&!log.healthy());mem.fail_write=-1;PersistentLog reboot(mem);assert(reboot.begin());auto got=page(reboot);assert(got.size()==1&&got[0].seq==1);
  }
  Memory mem;PersistentLog log(mem);assert(log.begin()&&log.append({event(1)}));mem.fail_read=0;std::vector<Event> out{event(88)};assert(!log.recent(out)&&out.empty()&&!log.healthy());
  Memory bad(sector*sectors-1);PersistentLog invalid(bad);assert(!invalid.begin()&&!invalid.healthy());
  Memory unknown;unknown.bytes[0]=42;PersistentLog foreign(unknown);assert(!foreign.begin()&&!foreign.healthy()&&unknown.writes==0&&unknown.erase_count==0);
}
}
int main(){empty_and_persistence();rollover_and_rotation();extended_query_type_bounds();newest_of_type();durable_clear();failed_full_clear_preserves_history();interrupted_recycled_sector_erase();damaged_records_and_invalid_input();errors_and_torn_writes();std::cout<<"persistent log: recovery, wear, pagination and fail-closed checks passed\n";}
