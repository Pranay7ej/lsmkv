// lsmkv: a small log-structured merge-tree key-value store.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

struct Options {
  bool create_if_missing = true;
  bool error_if_exists = false;
  // fsync the WAL on every write. Off means a crash can lose the last few
  // writes, but never leaves the DB half-applied.
  bool sync = false;

  size_t write_buffer_size = 4 << 20;      // memtable size before it's flushed
  size_t block_size = 4096;                // uncompressed bytes per data block
  int block_restart_interval = 16;         // keys between full (non prefix-compressed) keys
  int bloom_bits_per_key = 10;             // 0 disables bloom filters (~1% FP at 10)
  size_t block_cache_bytes = 8 << 20;
  size_t target_file_size = 2 << 20;       // compaction output files are cut at this size
  uint64_t max_bytes_for_level_base = 10 << 20;  // L1 size; each level below is 10x
  int l0_compaction_trigger = 4;
  int l0_stop_writes_trigger = 12;
  int max_open_files = 500;
};

class Snapshot;

struct ReadOptions {
  const Snapshot* snapshot = nullptr;  // null = latest
  bool fill_cache = true;
  bool verify_checksums = true;
};

class WriteBatch {
 public:
  WriteBatch();
  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();
  size_t Count() const;
  size_t ApproximateSize() const { return rep_.size(); }

 private:
  friend class WriteBatchInternal;
  std::string rep_;  // [seq u64][count u32] then records
};

class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;  // first key >= target
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual Status status() const = 0;
};

class DB {
 public:
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);
  virtual ~DB() = default;

  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;
  // All or nothing, also across crashes.
  virtual Status Write(WriteBatch* batch) = 0;
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;
  Status Get(std::string_view key, std::string* value) { return Get(ReadOptions(), key, value); }

  // Sorted scan. The iterator sees a consistent snapshot of the DB.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // Flush the memtable and wait for background compactions to settle.
  virtual Status CompactAll() = 0;
  virtual Status Flush() = 0;

  // "lsmkv.stats": per-level files/bytes and write amplification.
  // "lsmkv.num-files-at-level<N>".
  virtual bool GetProperty(std::string_view name, std::string* value) = 0;
};

// Deletes all files of the DB at path.
Status DestroyDB(const std::string& path);

}  // namespace lsmkv
