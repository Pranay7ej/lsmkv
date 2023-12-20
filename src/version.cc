#include "version.h"

#include <algorithm>

#include "coding.h"
#include "file.h"

namespace lsmkv {
namespace {

enum Tag : uint32_t {
  kLogNumber = 1,
  kNextFile = 2,
  kLastSequence = 3,
  kDeletedFile = 4,
  kNewFile = 5,
  kCompactPointer = 6,
};

std::string_view UserKey(const std::string& ikey) { return ExtractUserKey(ikey); }

bool FileOverlaps(const FileMetaData& f, std::string_view begin, std::string_view end) {
  return !(UserKey(f.largest) < begin || UserKey(f.smallest) > end);
}

// Iterates one sorted, non-overlapping level file by file.
class LevelIterator : public InternalIterator {
 public:
  LevelIterator(TableCache* tc, const ReadOptions& ro, std::vector<FilePtr> files)
      : tc_(tc), ro_(ro), files_(std::move(files)) {}

  bool Valid() const override { return iter_ && iter_->Valid(); }
  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override {
    if (!status_.ok()) return status_;
    return iter_ ? iter_->status() : Status::OK();
  }

  void SeekToFirst() override {
    Open(0);
    if (iter_) iter_->SeekToFirst();
    SkipEmpty();
  }
  void Seek(std::string_view target) override {
    auto it = std::lower_bound(files_.begin(), files_.end(), target, [](const FilePtr& f, std::string_view t) {
      return CompareInternalKey(f->largest, t) < 0;
    });
    Open(size_t(it - files_.begin()));
    if (iter_) iter_->Seek(target);
    SkipEmpty();
  }
  void Next() override {
    iter_->Next();
    SkipEmpty();
  }

 private:
  void Open(size_t i) {
    idx_ = i;
    iter_.reset();
    table_.reset();
    if (i >= files_.size()) return;
    Status s = tc_->FindTable(files_[i]->number, &table_);
    if (!s.ok()) {
      status_ = s;
      return;
    }
    iter_ = table_->NewIterator(ro_);
  }
  void SkipEmpty() {
    while (iter_ && !iter_->Valid() && iter_->status().ok()) {
      Open(idx_ + 1);
      if (iter_) iter_->SeekToFirst();
    }
  }

  TableCache* tc_;
  ReadOptions ro_;
  std::vector<FilePtr> files_;
  size_t idx_ = 0;
  std::shared_ptr<Table> table_;  // keeps the table alive under iter_
  std::unique_ptr<InternalIterator> iter_;
  Status status_;
};

// A table iterator that also owns its table.
class OwningTableIterator : public InternalIterator {
 public:
  OwningTableIterator(std::shared_ptr<Table> t, const ReadOptions& ro) : table_(std::move(t)), it_(table_->NewIterator(ro)) {}
  bool Valid() const override { return it_->Valid(); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  void Seek(std::string_view k) override { it_->Seek(k); }
  void Next() override { it_->Next(); }
  std::string_view key() const override { return it_->key(); }
  std::string_view value() const override { return it_->value(); }
  Status status() const override { return it_->status(); }

 private:
  std::shared_ptr<Table> table_;
  std::unique_ptr<InternalIterator> it_;
};

}  // namespace

// ------------------------------------------------------------ VersionEdit

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_) {
    PutVarint32(dst, kNextFile);
    PutVarint64(dst, next_file_);
  }
  if (has_last_seq_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_seq_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, uint32_t(level));
    PutLengthPrefixed(dst, key);
  }
  for (const auto& [level, number] : deleted_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, uint32_t(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, f] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, uint32_t(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixed(dst, f.smallest);
    PutLengthPrefixed(dst, f.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view in) {
  *this = VersionEdit();
  uint32_t tag, level;
  std::string_view s1, s2;
  while (!in.empty()) {
    if (!GetVarint32(&in, &tag)) return Status::Corruption("manifest: bad tag");
    bool ok = true;
    switch (tag) {
      case kLogNumber:
        ok = GetVarint64(&in, &log_number_);
        has_log_number_ = true;
        break;
      case kNextFile:
        ok = GetVarint64(&in, &next_file_);
        has_next_file_ = true;
        break;
      case kLastSequence:
        ok = GetVarint64(&in, &last_seq_);
        has_last_seq_ = true;
        break;
      case kCompactPointer:
        ok = GetVarint32(&in, &level) && level < kNumLevels && GetLengthPrefixed(&in, &s1);
        if (ok) compact_pointers_.emplace_back(int(level), std::string(s1));
        break;
      case kDeletedFile: {
        uint64_t n;
        ok = GetVarint32(&in, &level) && level < kNumLevels && GetVarint64(&in, &n);
        if (ok) deleted_.insert({int(level), n});
        break;
      }
      case kNewFile: {
        FileMetaData f;
        ok = GetVarint32(&in, &level) && level < kNumLevels && GetVarint64(&in, &f.number) &&
             GetVarint64(&in, &f.file_size) && GetLengthPrefixed(&in, &s1) && GetLengthPrefixed(&in, &s2) &&
             s1.size() >= 8 && s2.size() >= 8;
        if (ok) {
          f.smallest = std::string(s1);
          f.largest = std::string(s2);
          new_files_.emplace_back(int(level), std::move(f));
        }
        break;
      }
      default:
        return Status::Corruption("manifest: unknown tag");
    }
    if (!ok) return Status::Corruption("manifest: truncated edit");
  }
  return Status::OK();
}

// ------------------------------------------------------------ Version

uint64_t Version::LevelBytes(int level) const {
  uint64_t n = 0;
  for (const auto& f : files_[level]) n += f->file_size;
  return n;
}

void Version::GetOverlapping(int level, std::string_view begin, std::string_view end,
                             std::vector<FilePtr>* out) const {
  out->clear();
  for (const auto& f : files_[level])
    if (FileOverlaps(*f, begin, end)) out->push_back(f);
}

bool Version::OverlapsLevel(int level, std::string_view begin, std::string_view end) const {
  for (const auto& f : files_[level])
    if (FileOverlaps(*f, begin, end)) return true;
  return false;
}

Status Version::Get(const ReadOptions& ro, std::string_view user_key, SequenceNumber seq,
                    std::string* value) const {
  const std::string lookup = LookupKey(user_key, seq);
  enum { kNotSeen, kFound, kDeleted } state = kNotSeen;
  auto on_entry = [&](std::string_view k, std::string_view v) {
    ParsedInternalKey p;
    if (!ParseInternalKey(k, &p) || p.user_key != user_key) return;
    if (p.type == kTypeValue) {
      value->assign(v.data(), v.size());
      state = kFound;
    } else {
      state = kDeleted;
    }
  };

  auto search = [&](const FilePtr& f) -> Status {
    std::shared_ptr<Table> t;
    Status s = table_cache_->FindTable(f->number, &t);
    if (!s.ok()) return s;
    return t->Get(ro, lookup, on_entry);
  };

  // L0 newest first: the first file that knows about the key has the answer.
  for (const auto& f : files_[0]) {
    if (!FileOverlaps(*f, user_key, user_key)) continue;
    Status s = search(f);
    if (!s.ok()) return s;
    if (state != kNotSeen) break;
  }
  for (int level = 1; level < kNumLevels && state == kNotSeen; ++level) {
    const auto& files = files_[level];
    auto it = std::lower_bound(files.begin(), files.end(), lookup, [](const FilePtr& f, const std::string& k) {
      return CompareInternalKey(f->largest, k) < 0;
    });
    if (it == files.end() || UserKey((*it)->smallest) > user_key) continue;
    Status s = search(*it);
    if (!s.ok()) return s;
  }
  return state == kFound ? Status::OK() : Status::NotFound();
}

void Version::AddIterators(const ReadOptions& ro, std::vector<std::unique_ptr<InternalIterator>>* out) const {
  for (const auto& f : files_[0]) {
    std::shared_ptr<Table> t;
    Status s = table_cache_->FindTable(f->number, &t);
    if (!s.ok()) {
      out->push_back(std::make_unique<EmptyIterator>(s));
      continue;
    }
    out->push_back(std::make_unique<OwningTableIterator>(std::move(t), ro));
  }
  for (int level = 1; level < kNumLevels; ++level) {
    if (!files_[level].empty())
      out->push_back(std::make_unique<LevelIterator>(table_cache_, ro, files_[level]));
  }
}

// ------------------------------------------------------------ Compaction

bool Compaction::IsBaseLevelForKey(std::string_view user_key) const {
  for (int lvl = level + 2; lvl < kNumLevels; ++lvl)
    if (input_version->OverlapsLevel(lvl, user_key, user_key)) return false;
  return true;
}

// ------------------------------------------------------------ VersionSet

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  uint64_t r = options_.max_bytes_for_level_base;
  for (int l = 1; l < level; ++l) r *= 10;
  return r;
}

void VersionSet::Finalize(Version* v) const {
  int best = -1;
  double best_score = -1;
  for (int level = 0; level < kNumLevels - 1; ++level) {
    double score = level == 0 ? double(v->files_[0].size()) / options_.l0_compaction_trigger
                              : double(v->LevelBytes(level)) / double(MaxBytesForLevel(level));
    if (score > best_score) {
      best_score = score;
      best = level;
    }
  }
  v->compaction_level_ = best;
  v->compaction_score_ = best_score;
}

std::shared_ptr<Version> VersionSet::Apply(const VersionEdit& edit) const {
  auto v = std::make_shared<Version>(table_cache_);
  for (int level = 0; level < kNumLevels; ++level) {
    if (current_) {
      for (const auto& f : current_->files_[level])
        if (!edit.deleted_.count({level, f->number})) v->files_[level].push_back(f);
    }
  }
  for (const auto& [level, f] : edit.new_files_) {
    if (!edit.deleted_.count({level, f.number})) v->files_[level].push_back(std::make_shared<FileMetaData>(f));
  }
  std::sort(v->files_[0].begin(), v->files_[0].end(),
            [](const FilePtr& a, const FilePtr& b) { return a->number > b->number; });
  for (int level = 1; level < kNumLevels; ++level) {
    std::sort(v->files_[level].begin(), v->files_[level].end(),
              [](const FilePtr& a, const FilePtr& b) { return CompareInternalKey(a->smallest, b->smallest) < 0; });
  }
  Finalize(v.get());
  return v;
}

Status VersionSet::WriteSnapshot(LogWriter* w) const {
  VersionEdit e;
  e.SetLogNumber(log_number_);
  e.SetNextFile(next_file_);
  e.SetLastSequence(last_seq_);
  for (int level = 0; level < kNumLevels; ++level) {
    if (!compact_pointer_[level].empty()) e.SetCompactPointer(level, compact_pointer_[level]);
    if (current_)
      for (const auto& f : current_->files_[level]) e.AddFile(level, *f);
  }
  std::string rec;
  e.EncodeTo(&rec);
  return w->AddRecord(rec);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (edit->has_log_number_) log_number_ = edit->log_number_;
  edit->SetLogNumber(log_number_);
  edit->SetNextFile(next_file_);
  edit->SetLastSequence(last_seq_);

  auto v = Apply(*edit);

  std::string new_manifest;
  if (!manifest_) {
    // First change since open: start a fresh MANIFEST holding the full state.
    manifest_number_ = NewFileNumber();
    edit->SetNextFile(next_file_);
    new_manifest = ManifestFileName(dir_, manifest_number_);
    std::unique_ptr<WritableFile> f;
    Status s = WritableFile::Open(new_manifest, &f);
    if (!s.ok()) return s;
    manifest_ = std::make_unique<LogWriter>(std::move(f));
    s = WriteSnapshot(manifest_.get());
    if (!s.ok()) return s;
  }
  std::string rec;
  edit->EncodeTo(&rec);
  Status s = manifest_->AddRecord(rec);
  if (s.ok()) s = manifest_->Sync();
  if (s.ok() && !new_manifest.empty()) {
    // Point CURRENT at the new manifest atomically: write a temp file, rename.
    char name[32];
    snprintf(name, sizeof name, "MANIFEST-%06llu\n", static_cast<unsigned long long>(manifest_number_));
    const std::string tmp = dir_ + "/CURRENT.tmp";
    s = WriteStringToFileSync(tmp, name);
    if (s.ok()) s = RenameFile(tmp, CurrentFileName(dir_));
    if (s.ok()) s = SyncDir(dir_);
  }
  if (!s.ok()) {
    // Can't trust the manifest any more; the next change starts a new one.
    manifest_.reset();
    return s;
  }
  for (const auto& [level, key] : edit->compact_pointers_) compact_pointer_[level] = key;
  current_ = v;
  live_.push_back(v);
  return Status::OK();
}

Status VersionSet::CreateNew() {
  VersionEdit e;
  e.SetLogNumber(0);
  return LogAndApply(&e);
}

Status VersionSet::Recover(bool* exists) {
  std::string current;
  if (!FileExists(CurrentFileName(dir_))) {
    *exists = false;
    return Status::OK();
  }
  *exists = true;
  Status s = ReadFileToString(CurrentFileName(dir_), &current);
  if (!s.ok()) return s;
  if (current.empty() || current.back() != '\n') return Status::Corruption("CURRENT is malformed");
  current.pop_back();
  unsigned long long mnum = 0;
  if (sscanf(current.c_str(), "MANIFEST-%llu", &mnum) != 1) return Status::Corruption("CURRENT is malformed");

  std::unique_ptr<LogReader> reader;
  s = LogReader::Open(dir_ + "/" + current, &reader);
  if (!s.ok()) return s;
  std::string_view rec;
  bool have_next = false, have_log = false, have_seq = false;
  while (reader->ReadRecord(&rec)) {
    VersionEdit e;
    s = e.DecodeFrom(rec);
    if (!s.ok()) return s;
    current_ = Apply(e);
    for (const auto& [level, key] : e.compact_pointers_) compact_pointer_[level] = key;
    if (e.has_log_number_) log_number_ = e.log_number_, have_log = true;
    if (e.has_next_file_) next_file_ = e.next_file_, have_next = true;
    if (e.has_last_seq_) last_seq_ = e.last_seq_, have_seq = true;
  }
  // A torn last record just means that edit never committed; anything else is real damage.
  if (reader->hit_bad_checksum()) return Status::Corruption("manifest checksum mismatch");
  if (!have_next || !have_log || !have_seq) return Status::Corruption("manifest is missing fields");
  if (!current_) current_ = Apply(VersionEdit());
  live_.push_back(current_);
  manifest_number_ = mnum;
  next_file_ = std::max<uint64_t>(next_file_, mnum + 1);
  return Status::OK();
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (auto it = live_.begin(); it != live_.end();) {
    if (auto v = it->lock()) {
      for (const auto& level : v->files_)
        for (const auto& f : level) live->insert(f->number);
      ++it;
    } else {
      it = live_.erase(it);
    }
  }
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  std::string_view lo = UserKey(c->inputs[0].front()->smallest);
  std::string_view hi = UserKey(c->inputs[0].front()->largest);
  for (const auto& f : c->inputs[0]) {
    lo = std::min(lo, UserKey(f->smallest));
    hi = std::max(hi, UserKey(f->largest));
  }
  c->input_version->GetOverlapping(c->level + 1, lo, hi, &c->inputs[1]);
  c->trivial_move = c->inputs[0].size() == 1 && c->inputs[1].empty();
  compact_pointer_[c->level] = c->inputs[0].back()->largest;
  c->edit.SetCompactPointer(c->level, compact_pointer_[c->level]);
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() {
  if (!NeedsCompaction()) return nullptr;
  auto c = std::make_unique<Compaction>();
  c->level = current_->compaction_level_;
  c->input_version = current_;
  const auto& files = current_->files_[c->level];
  if (c->level == 0) {
    c->inputs[0] = files;  // L0 files overlap each other; take them all
  } else {
    // Round-robin through the key space so every file gets its turn.
    FilePtr pick = files.front();
    for (const auto& f : files) {
      if (compact_pointer_[c->level].empty() || CompareInternalKey(f->largest, compact_pointer_[c->level]) > 0) {
        pick = f;
        break;
      }
    }
    c->inputs[0].push_back(pick);
  }
  SetupOtherInputs(c.get());
  return c;
}

std::unique_ptr<Compaction> VersionSet::CompactWholeLevel(int level) {
  if (level < 0 || level >= kNumLevels - 1 || current_->files_[level].empty()) return nullptr;
  auto c = std::make_unique<Compaction>();
  c->level = level;
  c->input_version = current_;
  c->inputs[0] = current_->files_[level];
  SetupOtherInputs(c.get());
  c->trivial_move = false;  // manual compactions always rewrite, to drop garbage
  return c;
}

}  // namespace lsmkv
