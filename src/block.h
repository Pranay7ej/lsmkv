// Sorted key/value blocks with prefix compression.
//
// entry    := shared varint | non_shared varint | value_len varint | key_delta | value
// trailer  := restart_offset u32 ... | num_restarts u32
//
// Every `restart_interval` keys the full key is stored (shared = 0), and its
// offset recorded, so Seek can binary search the restarts and then scan.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "internal_iterator.h"

namespace lsmkv {

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval) : interval_(restart_interval) { restarts_.push_back(0); }

  // Keys must be added in increasing internal-key order.
  void Add(std::string_view key, std::string_view value);
  std::string_view Finish();
  void Reset();
  size_t EstimatedSize() const { return buf_.size() + restarts_.size() * 4 + 4; }
  bool empty() const { return buf_.empty(); }

 private:
  int interval_;
  std::string buf_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  std::string last_key_;
  bool finished_ = false;
};

using BlockContents = std::shared_ptr<const std::string>;

// Returns an iterator over the block; keeps `contents` alive.
std::unique_ptr<InternalIterator> NewBlockIterator(BlockContents contents);

}  // namespace lsmkv
