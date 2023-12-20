#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <list>
#include <mutex>
#include <set>
#include <thread>

#include <unistd.h>

#include "file.h"
#include "iterators.h"
#include "log.h"
#include "lsmkv/db.h"
#include "memtable.h"
#include "table.h"
#include "table_cache.h"
#include "version.h"
#include "write_batch.h"

namespace lsmkv {

class Snapshot {
 public:
  explicit Snapshot(SequenceNumber s) : seq(s) {}
  const SequenceNumber seq;
};

namespace {

// User-facing iterator: collapses internal entries into the newest visible
// version of each key at the snapshot, and hides deletions.
class DBIter : public Iterator {
 public:
  DBIter(std::unique_ptr<InternalIterator> it, SequenceNumber seq, std::shared_ptr<void> pins)
      : it_(std::move(it)), seq_(seq), pins_(std::move(pins)) {}

  bool Valid() const override { return valid_; }
  std::string_view key() const override { return ExtractUserKey(it_->key()); }
  std::string_view value() const override { return it_->value(); }
  Status status() const override { return it_->status(); }

  void SeekToFirst() override {
    it_->SeekToFirst();
    FindNextVisible(false);
  }
  void Seek(std::string_view target) override {
    it_->Seek(LookupKey(target, seq_));
    FindNextVisible(false);
  }
  void Next() override {
    // Remember the current key so we skip its older versions.
    skip_.assign(key().data(), key().size());
    it_->Next();
    FindNextVisible(true);
  }

 private:
  void FindNextVisible(bool skipping) {
    valid_ = false;
    for (; it_->Valid(); it_->Next()) {
      ParsedInternalKey p;
      if (!ParseInternalKey(it_->key(), &p)) continue;
      if (p.sequence > seq_) continue;  // written after our snapshot
      if (skipping && p.user_key == skip_) continue;
      if (p.type == kTypeDeletion) {
        // Newest visible version is a tombstone: hide it and everything older.
        skip_.assign(p.user_key.data(), p.user_key.size());
        skipping = true;
        continue;
      }
      valid_ = true;
      return;
    }
  }

  std::unique_ptr<InternalIterator> it_;
  SequenceNumber seq_;
  std::shared_ptr<void> pins_;  // memtables + version the iterator reads from
  std::string skip_;
  bool valid_ = false;
};

struct LevelStats {
  uint64_t bytes_read = 0, bytes_written = 0, count = 0;
};

}  // namespace

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, std::string dir)
      : options_(options),
        dir_(std::move(dir)),
        block_cache_(options.block_cache_bytes),
        table_cache_(dir_, options, &block_cache_),
        versions_(dir_, options, &table_cache_),
        mem_(std::make_shared<MemTable>()) {}

  ~DBImpl() override {
    {
      std::unique_lock<std::mutex> lk(mu_);
      shutting_down_ = true;
      bg_cv_.notify_all();
    }
    if (bg_thread_.joinable()) bg_thread_.join();
    if (log_) log_->Close();
  }

  Status Open();

  Status Put(std::string_view key, std::string_view value) override {
    WriteBatch b;
    b.Put(key, value);
    return Write(&b);
  }
  Status Delete(std::string_view key) override {
    WriteBatch b;
    b.Delete(key);
    return Write(&b);
  }
  Status Write(WriteBatch* batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* s) override;
  Status CompactAll() override;
  Status Flush() override;
  bool GetProperty(std::string_view name, std::string* value) override;

 private:
  Status Recover();
  Status ReplayLog(uint64_t number, SequenceNumber* max_seq, VersionEdit* edit);
  // Builds an L0 table from a memtable. Touches no shared state, so it can
  // run without mu_; the caller reserves the file number.
  Status WriteLevel0Table(const MemTable& mem, uint64_t number, VersionEdit* edit, uint64_t* size);
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lk, bool force);
  Status NewLogFile();
  void BackgroundLoop();
  void CompactMemTable(std::unique_lock<std::mutex>& lk);
  Status DoCompaction(std::unique_lock<std::mutex>& lk, Compaction* c);
  void DeleteObsoleteFiles();
  SequenceNumber SmallestSnapshot() const {
    return snapshots_.empty() ? versions_.last_sequence() : (*std::min_element(
        snapshots_.begin(), snapshots_.end(), [](const auto& a, const auto& b) { return a->seq < b->seq; }))->seq;
  }
  void RecordBgError(const Status& s) {
    if (bg_error_.ok()) bg_error_ = s;
    work_done_cv_.notify_all();
  }

  const Options options_;
  const std::string dir_;
  std::unique_ptr<FileLock> lock_;
  BlockCache block_cache_;
  TableCache table_cache_;

  std::mutex mu_;
  std::condition_variable bg_cv_;         // wakes the background thread
  std::condition_variable work_done_cv_;  // wakes writers waiting on flush/compaction
  VersionSet versions_;
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // being flushed to L0
  std::unique_ptr<LogWriter> log_;
  uint64_t log_number_ = 0;
  std::list<std::unique_ptr<Snapshot>> snapshots_;
  std::set<uint64_t> pending_outputs_;  // tables being written, not yet in a Version
  int manual_level_ = -1;
  bool bg_busy_ = false;
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> has_imm_{false};  // lets compaction check for a pending flush without the lock
  Status bg_error_;
  std::thread bg_thread_;

  // Stats.
  uint64_t user_bytes_ = 0;
  uint64_t flush_bytes_ = 0;
  std::array<LevelStats, kNumLevels> level_stats_{};
};

// ------------------------------------------------------------------ open

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->Open();
  if (s.ok()) *db = std::move(impl);
  return s;
}

Status DBImpl::Open() {
  Status s = CreateDirIfMissing(dir_);
  if (!s.ok()) return s;
  s = FileLock::Acquire(LockFileName(dir_), &lock_);
  if (!s.ok()) return s;
  s = Recover();
  if (!s.ok()) return s;
  bg_thread_ = std::thread([this] { BackgroundLoop(); });
  return Status::OK();
}

Status DBImpl::Recover() {
  std::unique_lock<std::mutex> lk(mu_);
  bool exists = false;
  Status s = versions_.Recover(&exists);
  if (!s.ok()) return s;
  if (exists && options_.error_if_exists) return Status::InvalidArgument(dir_ + " exists");
  if (!exists) {
    if (!options_.create_if_missing) return Status::InvalidArgument(dir_ + " does not exist");
    s = versions_.CreateNew();
    if (!s.ok()) return s;
  }

  // Replay every WAL at or after the one the manifest says is still live, oldest first.
  std::vector<std::string> names;
  s = GetChildren(dir_, &names);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  for (const auto& n : names) {
    unsigned long long num;
    char tail[8];
    if (sscanf(n.c_str(), "%llu.%7s", &num, tail) == 2 && std::string(tail) == "log" && num >= versions_.log_number())
      logs.push_back(num);
  }
  std::sort(logs.begin(), logs.end());
  VersionEdit edit;
  SequenceNumber max_seq = versions_.last_sequence();
  for (uint64_t n : logs) {
    s = ReplayLog(n, &max_seq, &edit);
    if (!s.ok()) return s;
    // Don't reuse the number for anything else.
    while (versions_.next_file_number() <= n) versions_.NewFileNumber();
  }
  versions_.SetLastSequence(max_seq);

  s = NewLogFile();
  if (!s.ok()) return s;
  edit.SetLogNumber(log_number_);  // older logs are now covered by L0 tables
  s = versions_.LogAndApply(&edit);
  if (!s.ok()) return s;
  DeleteObsoleteFiles();
  return Status::OK();
}

Status DBImpl::ReplayLog(uint64_t number, SequenceNumber* max_seq, VersionEdit* edit) {
  std::unique_ptr<LogReader> reader;
  Status s = LogReader::Open(LogFileName(dir_, number), &reader);
  if (!s.ok()) return s;
  auto mem = std::make_shared<MemTable>();
  std::string_view rec;
  WriteBatch batch;
  while (reader->ReadRecord(&rec)) {
    s = WriteBatchInternal::SetContents(&batch, rec);
    if (s.ok()) s = WriteBatchInternal::InsertInto(batch, mem.get());
    if (!s.ok()) return s;
    const SequenceNumber last = WriteBatchInternal::Sequence(batch) + WriteBatchInternal::Count(batch) - 1;
    *max_seq = std::max(*max_seq, last);
    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      uint64_t size;
      s = WriteLevel0Table(*mem, versions_.NewFileNumber(), edit, &size);
      if (!s.ok()) return s;
      mem = std::make_shared<MemTable>();
    }
  }
  if (reader->dropped_bytes() > 0) {
    // Expected after a crash mid-write: the tail record never fully made it to disk.
    fprintf(stderr, "lsmkv: %s: dropped %zu bytes at the end of the log%s\n", LogFileName(dir_, number).c_str(),
            reader->dropped_bytes(), reader->hit_bad_checksum() ? " (bad checksum)" : "");
  }
  if (mem->num_entries() > 0) {
    uint64_t size;
    s = WriteLevel0Table(*mem, versions_.NewFileNumber(), edit, &size);
  }
  return s;
}

Status DBImpl::NewLogFile() {
  const uint64_t n = versions_.NewFileNumber();
  std::unique_ptr<WritableFile> f;
  Status s = WritableFile::Open(LogFileName(dir_, n), &f);
  if (!s.ok()) return s;
  if (log_) log_->Close();
  log_ = std::make_unique<LogWriter>(std::move(f));
  log_number_ = n;
  return Status::OK();
}

// ------------------------------------------------------------------ writes

Status DBImpl::Write(WriteBatch* batch) {
  std::unique_lock<std::mutex> lk(mu_);
  Status s = MakeRoomForWrite(lk, false);
  if (!s.ok()) return s;
  if (WriteBatchInternal::Count(*batch) == 0) return Status::OK();
  const SequenceNumber seq = versions_.last_sequence() + 1;
  WriteBatchInternal::SetSequence(batch, seq);
  const std::string_view rep = WriteBatchInternal::Contents(*batch);
  s = log_->AddRecord(rep);
  if (s.ok()) s = options_.sync ? log_->Sync() : log_->Flush();
  if (!s.ok()) {
    RecordBgError(s);  // the WAL may now hold a partial record; stop accepting writes
    return s;
  }
  s = WriteBatchInternal::InsertInto(*batch, mem_.get());
  if (!s.ok()) return s;
  versions_.SetLastSequence(seq + WriteBatchInternal::Count(*batch) - 1);
  user_bytes_ += rep.size();
  return Status::OK();
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lk, bool force) {
  for (;;) {
    if (!bg_error_.ok()) return bg_error_;
    const size_t l0 = versions_.current()->files(0).size();
    if (!force && mem_->ApproximateMemoryUsage() < options_.write_buffer_size) return Status::OK();
    if (force && mem_->num_entries() == 0) return Status::OK();
    if (imm_ || l0 >= size_t(options_.l0_stop_writes_trigger)) {
      // Write stall: the previous memtable is still flushing, or L0 is too
      // deep and reads would suffer. Wait for the background thread.
      bg_cv_.notify_all();
      work_done_cv_.wait(lk);
      continue;
    }
    Status s = NewLogFile();
    if (!s.ok()) return s;
    imm_ = std::move(mem_);
    has_imm_.store(true, std::memory_order_relaxed);
    mem_ = std::make_shared<MemTable>();
    force = false;
    bg_cv_.notify_all();
    return Status::OK();
  }
}

// ------------------------------------------------------------------ reads

Status DBImpl::Get(const ReadOptions& ro, std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem, imm;
  VersionPtr version;
  SequenceNumber seq;
  {
    std::lock_guard<std::mutex> lk(mu_);
    mem = mem_;
    imm = imm_;
    version = versions_.current();
    seq = ro.snapshot ? ro.snapshot->seq : versions_.last_sequence();
  }
  // Memtable reads are lock-free (skiplist), and the Version is immutable.
  bool deleted = false;
  if (mem->Get(key, seq, value, &deleted) || (imm && imm->Get(key, seq, value, &deleted)))
    return deleted ? Status::NotFound() : Status::OK();
  return version->Get(ro, key, seq, value);
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& ro) {
  struct Pins {
    std::shared_ptr<MemTable> mem, imm;
    VersionPtr version;
  };
  auto pins = std::make_shared<Pins>();
  SequenceNumber seq;
  {
    std::lock_guard<std::mutex> lk(mu_);
    pins->mem = mem_;
    pins->imm = imm_;
    pins->version = versions_.current();
    seq = ro.snapshot ? ro.snapshot->seq : versions_.last_sequence();
  }
  std::vector<std::unique_ptr<InternalIterator>> children;
  children.push_back(pins->mem->NewIterator());
  if (pins->imm) children.push_back(pins->imm->NewIterator());
  pins->version->AddIterators(ro, &children);
  return std::make_unique<DBIter>(NewMergingIterator(std::move(children)), seq, pins);
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> lk(mu_);
  snapshots_.push_back(std::make_unique<Snapshot>(versions_.last_sequence()));
  return snapshots_.back().get();
}

void DBImpl::ReleaseSnapshot(const Snapshot* s) {
  std::lock_guard<std::mutex> lk(mu_);
  snapshots_.remove_if([s](const auto& p) { return p.get() == s; });
}

// ------------------------------------------------------------------ background work

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!shutting_down_) {
    if (!bg_error_.ok()) {
      bg_cv_.wait(lk);
      continue;
    }
    if (imm_) {
      bg_busy_ = true;
      CompactMemTable(lk);
    } else if (manual_level_ >= 0) {
      bg_busy_ = true;
      auto c = versions_.CompactWholeLevel(manual_level_);
      manual_level_ = -1;
      if (c) {
        Status s = DoCompaction(lk, c.get());
        if (!s.ok()) RecordBgError(s);
      }
    } else if (auto c = versions_.PickCompaction()) {
      bg_busy_ = true;
      Status s = DoCompaction(lk, c.get());
      if (!s.ok()) RecordBgError(s);
    } else {
      bg_busy_ = false;
      work_done_cv_.notify_all();
      bg_cv_.wait(lk);
      continue;
    }
    bg_busy_ = false;
    work_done_cv_.notify_all();
  }
}

Status DBImpl::WriteLevel0Table(const MemTable& mem, uint64_t number, VersionEdit* edit, uint64_t* size) {
  FileMetaData meta;
  meta.number = number;
  std::unique_ptr<WritableFile> f;
  Status s = WritableFile::Open(TableFileName(dir_, meta.number), &f);
  if (!s.ok()) return s;
  TableBuilder builder(options_, std::move(f));
  auto it = mem.NewIterator();
  it->SeekToFirst();
  if (it->Valid()) meta.smallest.assign(it->key().data(), it->key().size());
  for (; it->Valid(); it->Next()) {
    meta.largest.assign(it->key().data(), it->key().size());
    builder.Add(it->key(), it->value());
  }
  s = builder.Finish();
  if (!s.ok()) return s;
  meta.file_size = builder.FileSize();
  *size = meta.file_size;
  edit->AddFile(0, meta);
  return Status::OK();
}

void DBImpl::CompactMemTable(std::unique_lock<std::mutex>& lk) {
  std::shared_ptr<MemTable> imm = imm_;
  VersionEdit edit;
  const uint64_t number = versions_.NewFileNumber();
  pending_outputs_.insert(number);
  uint64_t size = 0;
  lk.unlock();
  // The flush itself runs without the lock; writers keep going into mem_.
  Status s = WriteLevel0Table(*imm, number, &edit, &size);
  lk.lock();
  pending_outputs_.erase(number);
  if (s.ok() && shutting_down_) s = Status::IOError("shutting down");
  if (s.ok()) {
    // Everything in logs before the current one is now safely in a table.
    edit.SetLogNumber(log_number_);
    s = versions_.LogAndApply(&edit);
  }
  if (s.ok()) {
    flush_bytes_ += size;
    level_stats_[0].bytes_written += size;
    imm_.reset();
    has_imm_.store(false, std::memory_order_relaxed);
    DeleteObsoleteFiles();
  } else if (!shutting_down_) {
    RecordBgError(s);
  }
}

Status DBImpl::DoCompaction(std::unique_lock<std::mutex>& lk, Compaction* c) {
  const int out_level = c->level + 1;
  if (c->trivial_move) {
    // Nothing to merge with below: just move the file down a level.
    const FileMetaData& f = *c->inputs[0][0];
    c->edit.RemoveFile(c->level, f.number);
    c->edit.AddFile(out_level, f);
    return versions_.LogAndApply(&c->edit);
  }

  const SequenceNumber smallest_snapshot = SmallestSnapshot();
  std::vector<std::unique_ptr<InternalIterator>> inputs;
  ReadOptions ro;
  ro.fill_cache = false;  // don't evict hot user data for a one-off scan
  uint64_t bytes_in = 0;
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs[which]) {
      std::shared_ptr<Table> t;
      Status s = table_cache_.FindTable(f->number, &t);
      if (!s.ok()) return s;
      struct Own : InternalIterator {
        std::shared_ptr<Table> t;
        std::unique_ptr<InternalIterator> it;
        bool Valid() const override { return it->Valid(); }
        void SeekToFirst() override { it->SeekToFirst(); }
        void Seek(std::string_view k) override { it->Seek(k); }
        void Next() override { it->Next(); }
        std::string_view key() const override { return it->key(); }
        std::string_view value() const override { return it->value(); }
        Status status() const override { return it->status(); }
      };
      auto own = std::make_unique<Own>();
      own->t = t;
      own->it = t->NewIterator(ro);
      inputs.push_back(std::move(own));
      bytes_in += f->file_size;
    }
  }

  std::vector<uint64_t> reserved;
  lk.unlock();

  auto merged = NewMergingIterator(std::move(inputs));
  std::unique_ptr<TableBuilder> builder;
  FileMetaData out;
  std::vector<FileMetaData> outputs;
  Status s;

  auto finish_output = [&]() -> Status {
    Status fs = builder->Finish();
    out.file_size = builder->FileSize();
    builder.reset();
    if (fs.ok()) outputs.push_back(out);
    return fs;
  };

  std::string current_user_key;
  bool has_current = false;
  SequenceNumber last_seq_for_key = kMaxSequenceNumber;

  for (merged->SeekToFirst(); merged->Valid() && s.ok(); merged->Next()) {
    if (shutting_down_.load(std::memory_order_relaxed)) {
      s = Status::IOError("shutting down");
      break;
    }
    if (has_imm_.load(std::memory_order_relaxed)) {
      // A full memtable waiting to be flushed beats compaction for priority,
      // otherwise writers stall behind us.
      lk.lock();
      if (imm_) CompactMemTable(lk);
      lk.unlock();
    }
    const std::string_view key = merged->key();
    ParsedInternalKey p;
    if (!ParseInternalKey(key, &p)) {
      s = Status::Corruption("bad internal key during compaction");
      break;
    }
    const bool new_user_key = !has_current || p.user_key != current_user_key;
    if (new_user_key) {
      current_user_key.assign(p.user_key.data(), p.user_key.size());
      has_current = true;
      last_seq_for_key = kMaxSequenceNumber;
    }

    bool drop = false;
    if (last_seq_for_key <= smallest_snapshot) {
      // A newer version of this key is already visible to every snapshot.
      drop = true;
    } else if (p.type == kTypeDeletion && p.sequence <= smallest_snapshot && c->IsBaseLevelForKey(p.user_key)) {
      // Tombstone with nothing older underneath to hide.
      drop = true;
    }
    last_seq_for_key = p.sequence;
    if (drop) continue;

    // Cut output files only at user-key boundaries, so a key's versions never
    // straddle two files in the same level.
    if (builder && new_user_key && builder->FileSize() >= options_.target_file_size) {
      s = finish_output();
      if (!s.ok()) break;
    }
    if (!builder) {
      {
        std::lock_guard<std::mutex> g(mu_);
        out = FileMetaData();
        out.number = versions_.NewFileNumber();
        pending_outputs_.insert(out.number);
        reserved.push_back(out.number);
      }
      std::unique_ptr<WritableFile> f;
      s = WritableFile::Open(TableFileName(dir_, out.number), &f);
      if (!s.ok()) break;
      builder = std::make_unique<TableBuilder>(options_, std::move(f));
      out.smallest.assign(key.data(), key.size());
    }
    out.largest.assign(key.data(), key.size());
    builder->Add(key, merged->value());
  }
  if (s.ok()) s = merged->status();
  if (s.ok() && builder) s = finish_output();
  if (builder) builder->Abandon();
  merged.reset();

  lk.lock();
  for (uint64_t n : reserved) pending_outputs_.erase(n);
  if (!s.ok()) {
    DeleteObsoleteFiles();  // clean up partial outputs
    return shutting_down_ ? Status::OK() : s;
  }
  uint64_t bytes_out = 0;
  for (int which = 0; which < 2; ++which)
    for (const auto& f : c->inputs[which]) c->edit.RemoveFile(c->level + which, f->number);
  for (const auto& o : outputs) {
    c->edit.AddFile(out_level, o);
    bytes_out += o.file_size;
  }
  s = versions_.LogAndApply(&c->edit);
  if (s.ok()) {
    level_stats_[out_level].bytes_read += bytes_in;
    level_stats_[out_level].bytes_written += bytes_out;
    level_stats_[out_level].count += 1;
    DeleteObsoleteFiles();
  }
  return s;
}

void DBImpl::DeleteObsoleteFiles() {
  std::set<uint64_t> live = pending_outputs_;
  versions_.AddLiveFiles(&live);
  std::vector<std::string> names;
  if (!GetChildren(dir_, &names).ok()) return;
  for (const auto& n : names) {
    unsigned long long num = 0;
    char tail[16] = {};
    bool keep = true;
    if (sscanf(n.c_str(), "MANIFEST-%llu", &num) == 1) {
      keep = num >= versions_.manifest_number();
    } else if (sscanf(n.c_str(), "%llu.%15s", &num, tail) == 2) {
      const std::string t = tail;
      if (t == "log") keep = num >= versions_.log_number() || num == log_number_;
      else if (t == "sst") keep = live.count(num) > 0;
    }
    if (!keep) {
      if (n.size() > 4 && n.compare(n.size() - 4, 4, ".sst") == 0) table_cache_.Evict(num);
      RemoveFile(dir_ + "/" + n);
    }
  }
}

// ------------------------------------------------------------------ manual

Status DBImpl::Flush() {
  std::unique_lock<std::mutex> lk(mu_);
  Status s = MakeRoomForWrite(lk, true);
  if (!s.ok()) return s;
  work_done_cv_.wait(lk, [&] { return !imm_ || !bg_error_.ok(); });
  return bg_error_;
}

Status DBImpl::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> lk(mu_);
  // Let automatic compactions settle first.
  work_done_cv_.wait(lk, [&] { return (!bg_busy_ && !versions_.NeedsCompaction() && !imm_) || !bg_error_.ok(); });
  int deepest = 0;
  for (int l = 0; l < kNumLevels; ++l)
    if (!versions_.current()->files(l).empty()) deepest = l;
  for (int l = 0; l < std::max(deepest, 1) && bg_error_.ok(); ++l) {
    if (versions_.current()->files(l).empty()) continue;
    manual_level_ = l;
    bg_cv_.notify_all();
    work_done_cv_.wait(lk, [&] { return (manual_level_ < 0 && !bg_busy_) || !bg_error_.ok(); });
  }
  return bg_error_;
}

bool DBImpl::GetProperty(std::string_view name, std::string* value) {
  std::lock_guard<std::mutex> lk(mu_);
  VersionPtr v = versions_.current();
  const std::string_view prefix = "lsmkv.num-files-at-level";
  if (name.substr(0, prefix.size()) == prefix) {
    const int level = std::atoi(std::string(name.substr(prefix.size())).c_str());
    if (level < 0 || level >= kNumLevels) return false;
    *value = std::to_string(v->files(level).size());
    return true;
  }
  if (name == "lsmkv.stats") {
    char buf[256];
    std::string out = "level  files    size MB   read MB  write MB  compactions\n";
    uint64_t written = 0;
    for (int l = 0; l < kNumLevels; ++l) {
      const auto& st = level_stats_[l];
      written += st.bytes_written;
      if (v->files(l).empty() && st.count == 0 && st.bytes_written == 0) continue;
      snprintf(buf, sizeof buf, "%5d %6zu %10.2f %9.2f %9.2f %12llu\n", l, v->files(l).size(),
               double(v->LevelBytes(l)) / 1048576.0, double(st.bytes_read) / 1048576.0,
               double(st.bytes_written) / 1048576.0, static_cast<unsigned long long>(st.count));
      out += buf;
    }
    snprintf(buf, sizeof buf,
             "user data written: %.2f MB, table bytes written: %.2f MB, write amplification: %.2f\n"
             "block cache: %.2f MB, hit rate %.1f%%\n",
             double(user_bytes_) / 1048576.0, double(written) / 1048576.0,
             user_bytes_ ? double(written) / double(user_bytes_) : 0.0, double(block_cache_.usage()) / 1048576.0,
             100.0 * double(block_cache_.hits()) / double(std::max<uint64_t>(1, block_cache_.hits() + block_cache_.misses())));
    out += buf;
    *value = out;
    return true;
  }
  if (name == "lsmkv.write-amplification") {
    uint64_t written = 0;
    for (const auto& st : level_stats_) written += st.bytes_written;
    char buf[32];
    snprintf(buf, sizeof buf, "%.3f", user_bytes_ ? double(written) / double(user_bytes_) : 0.0);
    *value = buf;
    return true;
  }
  return false;
}

Status DestroyDB(const std::string& path) {
  std::vector<std::string> names;
  if (!GetChildren(path, &names).ok()) return Status::OK();  // nothing there
  for (const auto& n : names) RemoveFile(path + "/" + n);
  ::rmdir(path.c_str());
  return Status::OK();
}

}  // namespace lsmkv
