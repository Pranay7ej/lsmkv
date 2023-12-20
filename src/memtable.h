#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "arena.h"
#include "dbformat.h"
#include "internal_iterator.h"
#include "skiplist.h"

namespace lsmkv {

// Entries are stored in the arena as:
//   varint32 internal_key_len | internal_key | varint32 value_len | value
class MemTable {
 public:
  MemTable() : table_(KeyComparator(), &arena_) {}

  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // Lookup at a sequence number. Returns true if the memtable has an answer:
  // *deleted = true for a tombstone, otherwise *value is filled in.
  bool Get(std::string_view user_key, SequenceNumber seq, std::string* value, bool* deleted) const;

  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }
  size_t num_entries() const { return entries_; }

  // Iterates internal keys. The memtable must outlive the iterator.
  std::unique_ptr<InternalIterator> NewIterator() const;

 private:
  struct KeyComparator {
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;
  class Iter;

  Arena arena_;
  Table table_;
  size_t entries_ = 0;
};

}  // namespace lsmkv
