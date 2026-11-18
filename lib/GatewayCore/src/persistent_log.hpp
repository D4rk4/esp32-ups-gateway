#pragma once

#include "events.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace gateway {
// Offsets are relative to the dedicated log partition. Writes must obey NOR
// flash semantics; erase always receives a single aligned 4096-byte sector.
class PersistentLogStorage {
 public:
  virtual ~PersistentLogStorage()=default;
  virtual size_t size() const=0;
  virtual bool read(size_t offset,void* destination,size_t length)=0;
  virtual bool write(size_t offset,const void* source,size_t length)=0;
  virtual bool erase(size_t offset,size_t length)=0;
};

// Calls must be serialized by the owner. A failed append may have committed a
// prefix; do not retry until reconstruction and reconciliation by sequence.
class PersistentLog {
 public:
  explicit PersistentLog(PersistentLogStorage& storage);
  bool begin();
  bool append(const std::vector<Event>& events);
  // The watermark includes issued events not yet persisted; 0 means none.
  bool clear(uint32_t sequence);
  // Newest first, bounded to 48; before is an exclusive sequence cursor.
  bool recent(std::vector<Event>& out,size_t limit=48,bool has_before=false,uint32_t before=0);
  // Newest stored event of one type, stopping at the first match. A pure query:
  // false when there is none or a read fails ("not known"), out is then
  // untouched and the log's health is unchanged; it never skips an unreadable
  // record to offer an older match.
  bool newest(EventType type,Event& out);
  uint32_t latestSequence() const {return latest_sequence_;}
  size_t count() const {return count_;}
  // One physical sector is reserved as a spare for safe clear/rotation.
  size_t capacity() const {return 3825;}
  bool healthy() const {return healthy_;}
 private:
  struct Sector {uint64_t generation{0};uint64_t epoch{0};uint32_t watermark{0},first{0},last{0};uint8_t count{0};bool valid{false};bool dirty{false};};
  bool scan();
  bool rotate(bool clear,uint32_t watermark);
  bool readRecord(size_t sector,size_t index,Event& event);
  bool walk(bool has_before,uint32_t before,const std::function<bool(Event&)>& visit);
  PersistentLogStorage& storage_;
  std::array<Sector,256> sectors_{};
  uint64_t generation_{0},epoch_{0};
  size_t current_{256},count_{0};
  uint32_t latest_sequence_{0};
  bool healthy_{false},initialized_{false};
};
}
