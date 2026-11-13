#include "event_bus.hpp"
#include <esp_timer.h>
#include <freertos/task.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
using namespace gateway;
namespace {
struct MemoryStorage:PersistentLogStorage {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(0x100000,0xff);
  size_t reads=0,writes=0,erases=0,fail_after_reads=SIZE_MAX,fail_after_writes=SIZE_MAX;bool fail_read=false,fail_write=false;std::function<void()> on_write;
  size_t size()const override{return bytes.size();}
  bool read(size_t offset,void* out,size_t size)override{
    if(fail_read||reads>=fail_after_reads||offset>bytes.size()||size>bytes.size()-offset)return false;
    ++reads;std::memcpy(out,bytes.data()+offset,size);return true;
  }
  bool write(size_t offset,const void* data,size_t size)override{
    if(on_write){auto callback=std::move(on_write);on_write=nullptr;callback();}
    if(fail_write||writes>=fail_after_writes||offset>bytes.size()||size>bytes.size()-offset)return false;
    const auto* in=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i)if((bytes[offset+i]&in[i])!=in[i])return false;
    for(size_t i=0;i<size;++i)bytes[offset+i]&=in[i];++writes;return true;
  }
  bool erase(size_t offset,size_t size)override{
    if(offset%4096||size!=4096||offset>bytes.size()||size>bytes.size()-offset)return false;
    std::fill(bytes.begin()+offset,bytes.begin()+offset+size,0xff);++erases;return true;
  }
};
void partitionBoundary(){
  std::string error;PersistentLogStore store;host_partition_present=false;
  assert(!store.begin(error)&&!error.empty());
  host_partition_present=true;host_partition.address=0x7f1000;
  assert(!store.begin(error));host_partition.address=0x7f0000;host_partition.size=0xff000;
  assert(!store.begin(error));host_partition.size=0x100000;
  host_partition.label[0]='x';assert(!store.begin(error));host_partition.label[0]='l';
  assert(store.begin(error)&&error.empty()&&store.size()==0x100000);
  uint8_t byte=0;
  assert(!store.read(0x100000,&byte,1)&&!store.write(0x100000,&byte,1));
  assert(!store.erase(1,4096)&&!store.erase(0,8192));
  assert(host_partition_reads==0&&host_partition_writes==0&&host_partition_erases==0);
  host_task_yields=0;EventBus bus;assert(bus.begin());
  assert(bus.persistenceStatus().ok&&host_partition_reads==4096&&host_task_yields==256);
  assert(bus.history().empty()&&host_partition_writes==0&&host_partition_erases==0);
  bus.publish(EventType::ThresholdRaised,1,"partition");assert(bus.flushPending(error));
  assert(host_partition_writes>0&&host_partition_erases==0);
  host_partition_fail_write=true;bus.publish(EventType::ThresholdRaised,1,"read-only");
  assert(!bus.flushPending(error)&&!bus.persistenceStatus().ok&&bus.persistenceStatus().pending==1);
  host_partition_fail_write=false;host_partition_present=false;
}
void durableEvents(){
  MemoryStorage storage;EventBus bus;host_time_us=0;assert(bus.begin(storage));
  assert(bus.persistenceStatus().ok&&bus.persistenceStatus().stored==0&&storage.writes==0&&storage.erases==0);
  bus.publish(EventType::LinkLost,3,"durable",123);
  assert(bus.persistenceStatus().pending==1&&bus.history().size()==1);
  assert(storage.writes==0&&storage.erases==0);
  host_time_us=29999000;bus.tick();assert(storage.writes==0);
  host_time_us=30000000;bus.tick();assert(bus.persistenceStatus().stored==1&&bus.persistenceStatus().pending==0);
  const auto writes=storage.writes,erases=storage.erases;
  host_time_us=90000000;bus.tick();assert(storage.writes==writes&&storage.erases==erases);
  EventBus restarted;assert(restarted.begin(storage));
  assert(restarted.latest().size()==1&&restarted.latest()[0].message=="durable"&&restarted.queued()==0);
  assert(restarted.latestSequence()==1);
  for(int i=0;i<16;++i)restarted.publish(EventType::ThresholdRaised,2,"batch");
  restarted.tick();assert(restarted.persistenceStatus().stored==17&&restarted.persistenceStatus().pending==0);
  const auto first=restarted.history(0,3);assert(first.size()==3&&first[0].seq==17&&first[2].seq==15);
  const auto second=restarted.history(first.back().seq,3);assert(second.size()==3&&second[0].seq==14);
  restarted.publish(EventType::LinkLost,3,"before clear");
  storage.on_write=[&]{restarted.publish(EventType::ThresholdRaised,2,"during clear");};
  std::string error;assert(restarted.clearLog(error));
  assert(restarted.history().size()==1&&restarted.history()[0].message=="during clear");
  assert(restarted.queued()==1&&restarted.persistenceStatus().pending==1);
  EventBus cleared;assert(cleared.begin(storage));assert(cleared.latestSequence()==18&&cleared.latest().empty());
  assert(restarted.flushPending(error));EventBus after;assert(after.begin(storage));
  assert(after.latest().size()==1&&after.latest()[0].seq==19&&after.latest()[0].message=="during clear");
}
void flushInterleavingAndPartialFailure(){
  MemoryStorage storage;EventBus bus;host_time_us=0;assert(bus.begin(storage));
  bus.publish(EventType::LinkLost,3,"first");
  storage.on_write=[&]{host_time_us=5000000;bus.publish(EventType::ThresholdRaised,2,"during flush");};
  std::string error;assert(bus.flushPending(error));
  const auto after_flush=bus.persistenceStatus();
  assert(after_flush.stored==1&&after_flush.pending==1&&bus.queued()==1);
  assert(bus.latestSequence()==2&&bus.history().size()==2);
  host_time_us=34999000;bus.tick();assert(bus.persistenceStatus().pending==1);
  host_time_us=35000000;bus.tick();assert(bus.persistenceStatus().pending==0&&bus.persistenceStatus().stored==2);

  bus.publish(EventType::ThresholdRaised,2,"partial one");
  bus.publish(EventType::ThresholdRaised,2,"partial two");
  storage.fail_after_writes=storage.writes+1;
  assert(!bus.flushPending(error)&&!error.empty());
  assert(!bus.persistenceStatus().ok&&bus.persistenceStatus().pending==2);
  const auto count=storage.writes,erases=storage.erases;
  assert(!bus.flushPending(error));bus.tick();assert(storage.writes==count&&storage.erases==erases);
  const auto history=bus.history();assert(history.size()==4&&history[0].seq==4&&history[1].seq==3);
  const auto older=bus.history(4,2);assert(older.size()==2&&older[0].seq==3&&older[1].seq==2);
  storage.fail_after_writes=SIZE_MAX;
  EventBus restarted;assert(restarted.begin(storage));
  assert(restarted.latestSequence()==3&&restarted.latest().size()==3&&restarted.queued()==0);
  MemoryStorage larger;EventBus long_history;assert(long_history.begin(larger));
  for(int i=0;i<64;++i){long_history.publish(EventType::ThresholdRaised,1,"batch");if(i%16==15)assert(long_history.flushPending(error));}
  long_history.publish(EventType::ThresholdRaised,1,"partial");
  long_history.publish(EventType::ThresholdRaised,1,"failed");
  larger.fail_after_writes=larger.writes+1;assert(!long_history.flushPending(error));
  const auto older_page=long_history.history(10,3);
  assert(older_page.size()==3&&older_page[0].seq==9&&older_page[2].seq==7);
  MemoryStorage overflow;EventBus dropped_history;assert(dropped_history.begin(overflow));
  for(int i=0;i<60;++i)dropped_history.publish(EventType::ThresholdRaised,1,"overflow");
  assert(dropped_history.persistenceStatus().dropped==12);
  assert(dropped_history.flushPending(error));
  const auto combined=dropped_history.history();
  assert(combined.size()==48&&combined.front().seq==60&&combined.back().seq==13);
  const auto remaining=dropped_history.history(13,12);
  assert(remaining.size()==12&&remaining.front().seq==12&&remaining.back().seq==1);
  MemoryStorage missing_partition;EventBus unavailable;missing_partition.fail_read=true;assert(unavailable.begin(missing_partition));
  for(int i=0;i<60;++i)unavailable.publish(EventType::ThresholdRaised,1,"ram only");
  const auto ram_only=unavailable.history(13,12);
  assert(ram_only.size()==12&&ram_only.front().seq==12&&ram_only.back().seq==1);
}
void readFailures(){
  MemoryStorage storage;EventBus bus;assert(bus.begin(storage));
  bus.publish(EventType::ThresholdRaised,1,"on disk");std::string error;assert(bus.flushPending(error));
  const auto writes=storage.writes,erases=storage.erases;
  assert(bus.history().size()==1&&storage.writes==writes&&storage.erases==erases);
  storage.fail_read=true;const auto available=bus.history();
  assert(available.size()==1&&available[0].message=="on disk");
  assert(!bus.persistenceStatus().ok&&!bus.persistenceStatus().error.empty());
  bus.publish(EventType::ThresholdRaised,1,"in ram");
  assert(!bus.flushPending(error)&&bus.persistenceStatus().pending==1);
  assert(storage.writes==writes&&storage.erases==erases);
  EventBus unreadable;assert(unreadable.begin(storage));
  assert(!unreadable.persistenceStatus().ok&&!unreadable.persistenceStatus().error.empty());
  assert(storage.writes==writes&&storage.erases==erases);
  storage.fail_read=false;storage.fail_after_reads=storage.reads+4096;
  EventBus recovery_failure;assert(recovery_failure.begin(storage));
  assert(!recovery_failure.persistenceStatus().ok&&recovery_failure.latestSequence()==1);
  recovery_failure.publish(EventType::ThresholdRaised,1,"after recovery failure");
  assert(recovery_failure.latestSequence()==2);
}
void failedStorage(){
  MemoryStorage storage;EventBus bus;assert(bus.begin(storage));bus.publish(EventType::LinkLost,3,"keep");
  storage.fail_write=true;std::string error;
  assert(!bus.clearLog(error)&&!error.empty()&&bus.latest().size()==1&&bus.queued()==1);
  assert(!bus.flushPending(error)&&!error.empty()&&bus.persistenceStatus().pending==1&&!bus.persistenceStatus().ok);
  const auto writes=storage.writes;bus.tick();assert(storage.writes==writes);
  for(int i=0;i<60;++i)bus.publish(EventType::ThresholdRaised,2,"overflow");
  assert(bus.persistenceStatus().pending==48&&bus.persistenceStatus().dropped==13);
}
}
int main(){
  EventBus bus;assert(bus.begin());
  bus.publish(EventType::LinkLost,3,"UPS link unavailable");
  bus.publish(EventType::ThresholdRaised,2,"Configured threshold exceeded");
  assert(bus.latest().size()==2&&bus.queued()==1);
  const auto sequence=bus.latest()[0].seq;
  std::string error;assert(!bus.clearLog(error));assert(!error.empty()&&bus.latest().size()==2&&bus.queued()==1);
  assert(!bus.flushPending(error)&&!error.empty());
  assert(!bus.persistenceStatus().ok&&bus.persistenceStatus().pending==2);
  assert(bus.latestSequence()==sequence&&bus.history().size()==2);
  Event pending;assert(bus.popTrap(pending)&&pending.type==EventType::LinkLost&&pending.severity==3);
  assert(pending.message=="UPS link unavailable"&&!bus.popTrap(pending));
  for(size_t i=0;i<60;++i)bus.publish(EventType::ThresholdRaised,2,"threshold");
  assert(bus.latest().size()==48&&bus.latest()[0].seq>sequence&&bus.queued()==0);
  partitionBoundary();durableEvents();flushInterleavingAndPartialFailure();readFailures();failedStorage();
  std::cout<<"event_bus platform boundary: durable journal, bounded queue, recovery and clear passed\n";
}
