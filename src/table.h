// Sorted string table.
//
//   [data block][trailer] ... [filter block][trailer] [index block][trailer] [footer]
//
// trailer: type u8 (0 = raw) | masked crc32c(block + type) u32
// index:   one entry per data block, key = last internal key in the block,
//          value = block handle (offset varint64, size varint64)
// filter:  bloom filter over every user key in the table
// footer:  filter handle | index handle, padded to 40 bytes | magic u64
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <atomic>

#include "block.h"
#include "bloom.h"
#include "cache.h"
#include "file.h"
#include "internal_iterator.h"
#include "lsmkv/db.h"

namespace lsmkv {

constexpr uint64_t kTableMagic = 0x6c736d6b7673737aULL;  // "lsmkvssz"
constexpr size_t kFooterSize = 48;
constexpr size_t kBlockTrailerSize = 5;

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // without the trailer
  void EncodeTo(std::string* dst) const;
  bool DecodeFrom(std::string_view* in);
};

class TableBuilder {
 public:
  TableBuilder(const Options& options, std::unique_ptr<WritableFile> file);

  // Internal keys, strictly increasing.
  void Add(std::string_view internal_key, std::string_view value);
  Status Finish();  // writes filter, index and footer, syncs and closes
  void Abandon();

  uint64_t FileSize() const { return offset_ + (data_.empty() ? 0 : data_.EstimatedSize()); }
  uint64_t NumEntries() const { return entries_; }
  Status status() const { return status_; }

 private:
  void FlushDataBlock();
  BlockHandle WriteRawBlock(std::string_view contents);

  Options options_;
  std::unique_ptr<WritableFile> file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_;
  BlockBuilder index_;
  std::unique_ptr<BloomBuilder> bloom_;  // null when bloom_bits_per_key == 0
  std::string last_key_;
  uint64_t entries_ = 0;
  bool closed_ = false;
};

using BlockCache = LruCache<const std::string>;

class Table {
 public:
  static Status Open(const Options& options, std::unique_ptr<RandomAccessFile> file, uint64_t file_number,
                     BlockCache* cache, std::unique_ptr<Table>* out);

  // Seeks to the first entry >= internal_key and, if its user key matches,
  // calls found(key, value). Returns early (no I/O on data blocks) when the
  // bloom filter rules the key out.
  template <typename Fn>
  Status Get(const ReadOptions& ro, std::string_view internal_key, Fn&& found) const;

  std::unique_ptr<InternalIterator> NewIterator(const ReadOptions& ro) const;

  // Checked by Get: how often the bloom filter saved a block read.
  uint64_t bloom_useful() const { return bloom_useful_.load(std::memory_order_relaxed); }
  size_t filter_size() const { return filter_.size(); }

  Status ReadBlock(const ReadOptions& ro, const BlockHandle& h, BlockContents* out) const;

 private:
  Table() = default;
  bool KeyMayMatch(std::string_view internal_key) const;

  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_number_ = 0;
  BlockCache* cache_ = nullptr;
  BlockContents index_;
  std::string filter_;
  mutable std::atomic<uint64_t> bloom_useful_{0};
};

template <typename Fn>
Status Table::Get(const ReadOptions& ro, std::string_view internal_key, Fn&& found) const {
  if (!KeyMayMatch(internal_key)) {
    bloom_useful_.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
  }
  auto index = NewBlockIterator(index_);
  index->Seek(internal_key);
  if (!index->Valid()) return index->status();
  BlockHandle h;
  std::string_view hv = index->value();
  if (!h.DecodeFrom(&hv)) return Status::Corruption("bad block handle");
  BlockContents block;
  Status s = ReadBlock(ro, h, &block);
  if (!s.ok()) return s;
  auto it = NewBlockIterator(block);
  it->Seek(internal_key);
  if (it->Valid()) found(it->key(), it->value());
  return it->status();
}

}  // namespace lsmkv
