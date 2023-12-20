// Keeps recently used tables open (file handle, index and bloom filter).
#pragma once

#include <memory>
#include <string>

#include "cache.h"
#include "table.h"

namespace lsmkv {

class TableCache {
 public:
  TableCache(std::string dir, const Options& options, BlockCache* block_cache)
      : dir_(std::move(dir)), options_(options), block_cache_(block_cache), tables_(size_t(options.max_open_files)) {}

  Status FindTable(uint64_t number, std::shared_ptr<Table>* out);
  void Evict(uint64_t number) { tables_.Erase(Key(number)); }

 private:
  static std::string Key(uint64_t n) { return std::to_string(n); }

  std::string dir_;
  Options options_;
  BlockCache* block_cache_;
  LruCache<Table> tables_;
};

inline Status TableCache::FindTable(uint64_t number, std::shared_ptr<Table>* out) {
  const std::string key = Key(number);
  if ((*out = tables_.Lookup(key))) return Status::OK();
  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(TableFileName(dir_, number), &file);
  if (!s.ok()) return s;
  std::unique_ptr<Table> t;
  s = Table::Open(options_, std::move(file), number, block_cache_, &t);
  if (!s.ok()) return s;
  *out = std::shared_ptr<Table>(std::move(t));
  tables_.Insert(key, *out, 1);
  return Status::OK();
}

}  // namespace lsmkv
