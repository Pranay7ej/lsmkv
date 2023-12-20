// Which table files make up the DB, per level, and how that changes over time.
//
// A Version is an immutable snapshot of the file set. Readers grab a
// shared_ptr to the current Version and can keep reading from it even while
// compactions install newer ones. Every change is a VersionEdit appended to
// the MANIFEST log, so the file set survives restarts.
#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dbformat.h"
#include "internal_iterator.h"
#include "log.h"
#include "lsmkv/db.h"
#include "table_cache.h"

namespace lsmkv {

constexpr int kNumLevels = 7;

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // internal keys
  std::string largest;
};
using FilePtr = std::shared_ptr<const FileMetaData>;

class VersionEdit {
 public:
  void SetLogNumber(uint64_t n) { has_log_number_ = true, log_number_ = n; }
  void SetNextFile(uint64_t n) { has_next_file_ = true, next_file_ = n; }
  void SetLastSequence(SequenceNumber s) { has_last_seq_ = true, last_seq_ = s; }
  void SetCompactPointer(int level, std::string key) { compact_pointers_.emplace_back(level, std::move(key)); }
  void AddFile(int level, const FileMetaData& f) { new_files_.emplace_back(level, f); }
  void RemoveFile(int level, uint64_t number) { deleted_.insert({level, number}); }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);

 private:
  friend class VersionSet;
  bool has_log_number_ = false, has_next_file_ = false, has_last_seq_ = false;
  uint64_t log_number_ = 0, next_file_ = 0;
  SequenceNumber last_seq_ = 0;
  std::vector<std::pair<int, std::string>> compact_pointers_;
  std::set<std::pair<int, uint64_t>> deleted_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

class Version {
 public:
  explicit Version(TableCache* tc) : table_cache_(tc) {}

  // Point lookup below the memtables. Returns NotFound if no level has the key.
  Status Get(const ReadOptions& ro, std::string_view user_key, SequenceNumber seq, std::string* value) const;

  // One iterator for each L0 file, one concatenating iterator per deeper level.
  void AddIterators(const ReadOptions& ro, std::vector<std::unique_ptr<InternalIterator>>* out) const;

  const std::vector<FilePtr>& files(int level) const { return files_[level]; }
  uint64_t LevelBytes(int level) const;
  // Files in `level` whose user-key range intersects [begin, end].
  void GetOverlapping(int level, std::string_view begin, std::string_view end, std::vector<FilePtr>* out) const;
  bool OverlapsLevel(int level, std::string_view begin, std::string_view end) const;

  int compaction_level() const { return compaction_level_; }
  double compaction_score() const { return compaction_score_; }

 private:
  friend class VersionSet;
  TableCache* table_cache_;
  // L0 sorted newest file first (files may overlap); other levels sorted by
  // smallest key and non-overlapping.
  std::array<std::vector<FilePtr>, kNumLevels> files_;
  int compaction_level_ = -1;
  double compaction_score_ = 0;
};
using VersionPtr = std::shared_ptr<const Version>;

struct Compaction {
  int level = 0;  // inputs[0] from level, inputs[1] from level + 1
  std::array<std::vector<FilePtr>, 2> inputs;
  VersionPtr input_version;
  bool trivial_move = false;
  // True if no level deeper than level+1 has data for user_key, so a
  // tombstone for it can be dropped.
  bool IsBaseLevelForKey(std::string_view user_key) const;
  VersionEdit edit;
};

class VersionSet {
 public:
  VersionSet(std::string dir, const Options& options, TableCache* tc)
      : dir_(std::move(dir)), options_(options), table_cache_(tc) {}

  // Loads CURRENT + MANIFEST. *exists = false if there's no DB yet.
  Status Recover(bool* exists);
  // Writes a brand new MANIFEST with the current state. Used on create.
  Status CreateNew();

  // Appends the edit to the MANIFEST and installs the resulting Version.
  // Caller holds the DB mutex.
  Status LogAndApply(VersionEdit* edit);

  VersionPtr current() const { return current_; }
  uint64_t NewFileNumber() { return next_file_++; }
  uint64_t next_file_number() const { return next_file_; }
  SequenceNumber last_sequence() const { return last_seq_; }
  void SetLastSequence(SequenceNumber s) { last_seq_ = s; }
  uint64_t log_number() const { return log_number_; }
  uint64_t manifest_number() const { return manifest_number_; }
  uint64_t MaxBytesForLevel(int level) const;

  bool NeedsCompaction() const { return current_->compaction_score_ >= 1; }
  std::unique_ptr<Compaction> PickCompaction();
  // Everything in `level` into level + 1.
  std::unique_ptr<Compaction> CompactWholeLevel(int level);

  // File numbers still referenced by any live Version (for GC).
  void AddLiveFiles(std::set<uint64_t>* live);

 private:
  void Finalize(Version* v) const;
  std::shared_ptr<Version> Apply(const VersionEdit& edit) const;
  void SetupOtherInputs(Compaction* c);
  Status WriteSnapshot(LogWriter* w) const;

  std::string dir_;
  Options options_;
  TableCache* table_cache_;
  VersionPtr current_;
  std::list<std::weak_ptr<const Version>> live_;  // for AddLiveFiles
  uint64_t next_file_ = 2;
  uint64_t manifest_number_ = 1;
  uint64_t log_number_ = 0;
  SequenceNumber last_seq_ = 0;
  std::array<std::string, kNumLevels> compact_pointer_;
  std::unique_ptr<LogWriter> manifest_;
};

}  // namespace lsmkv
