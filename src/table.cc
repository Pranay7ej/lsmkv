#include "table.h"

#include "coding.h"
#include "crc32c.h"
#include "dbformat.h"

namespace lsmkv {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

bool BlockHandle::DecodeFrom(std::string_view* in) { return GetVarint64(in, &offset) && GetVarint64(in, &size); }

// ---------------------------------------------------------------- builder

TableBuilder::TableBuilder(const Options& options, std::unique_ptr<WritableFile> file)
    : options_(options),
      file_(std::move(file)),
      data_(options.block_restart_interval),
      index_(1) {  // index keys share little; a restart per entry keeps Seek a pure binary search
  if (options.bloom_bits_per_key > 0) bloom_ = std::make_unique<BloomBuilder>(options.bloom_bits_per_key);
}

void TableBuilder::Add(std::string_view key, std::string_view value) {
  if (!status_.ok()) return;
  if (bloom_) bloom_->AddKey(ExtractUserKey(key));
  data_.Add(key, value);
  last_key_.assign(key.data(), key.size());
  ++entries_;
  if (data_.EstimatedSize() >= options_.block_size) FlushDataBlock();
}

BlockHandle TableBuilder::WriteRawBlock(std::string_view contents) {
  BlockHandle h;
  h.offset = offset_;
  h.size = contents.size();
  char trailer[kBlockTrailerSize];
  trailer[0] = 0;  // no compression
  uint32_t crc = crc32c::Value(contents.data(), contents.size());
  crc = crc32c::Extend(crc, trailer, 1);
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  status_ = file_->Append(contents);
  if (status_.ok()) status_ = file_->Append(std::string_view(trailer, kBlockTrailerSize));
  offset_ += contents.size() + kBlockTrailerSize;
  return h;
}

void TableBuilder::FlushDataBlock() {
  if (data_.empty() || !status_.ok()) return;
  BlockHandle h = WriteRawBlock(data_.Finish());
  data_.Reset();
  std::string hv;
  h.EncodeTo(&hv);
  index_.Add(last_key_, hv);
}

Status TableBuilder::Finish() {
  FlushDataBlock();
  BlockHandle filter_handle;
  if (bloom_) filter_handle = WriteRawBlock(bloom_->Finish());
  BlockHandle index_handle = WriteRawBlock(index_.Finish());
  if (!status_.ok()) return status_;

  std::string footer;
  filter_handle.EncodeTo(&footer);
  index_handle.EncodeTo(&footer);
  footer.resize(kFooterSize - 8, '\0');
  PutFixed64(&footer, kTableMagic);
  status_ = file_->Append(footer);
  offset_ += footer.size();
  if (status_.ok()) status_ = file_->Sync();
  if (status_.ok()) status_ = file_->Close();
  closed_ = true;
  return status_;
}

void TableBuilder::Abandon() {
  if (!closed_) file_->Close();
  closed_ = true;
}

// ---------------------------------------------------------------- reader

Status Table::Open(const Options& options, std::unique_ptr<RandomAccessFile> file, uint64_t file_number,
                   BlockCache* cache, std::unique_ptr<Table>* out) {
  (void)options;
  if (file->size() < kFooterSize) return Status::Corruption("table too short");
  std::string footer;
  Status s = file->Read(file->size() - kFooterSize, kFooterSize, &footer);
  if (!s.ok()) return s;
  if (DecodeFixed64(footer.data() + kFooterSize - 8) != kTableMagic) return Status::Corruption("bad table magic");
  std::string_view in(footer.data(), kFooterSize - 8);
  BlockHandle filter_h, index_h;
  if (!filter_h.DecodeFrom(&in) || !index_h.DecodeFrom(&in)) return Status::Corruption("bad footer");

  std::unique_ptr<Table> t(new Table());
  t->file_ = std::move(file);
  t->file_number_ = file_number;
  t->cache_ = cache;
  ReadOptions ro;
  ro.fill_cache = false;  // the index is pinned in the Table itself
  s = t->ReadBlock(ro, index_h, &t->index_);
  if (!s.ok()) return s;
  if (filter_h.size > 0) {
    BlockContents f;
    s = t->ReadBlock(ro, filter_h, &f);
    if (!s.ok()) return s;
    t->filter_ = *f;
  }
  *out = std::move(t);
  return Status::OK();
}

Status Table::ReadBlock(const ReadOptions& ro, const BlockHandle& h, BlockContents* out) const {
  std::string key;
  if (cache_) {
    key.reserve(16);
    PutFixed64(&key, file_number_);
    PutFixed64(&key, h.offset);
    if (auto hit = cache_->Lookup(key)) {
      *out = std::move(hit);
      return Status::OK();
    }
  }
  if (h.offset + h.size + kBlockTrailerSize > file_->size()) return Status::Corruption("block handle past EOF");
  auto buf = std::make_shared<std::string>();
  Status s = file_->Read(h.offset, h.size + kBlockTrailerSize, buf.get());
  if (!s.ok()) return s;
  const char* trailer = buf->data() + h.size;
  if (ro.verify_checksums) {
    uint32_t crc = crc32c::Extend(crc32c::Value(buf->data(), h.size), trailer, 1);
    if (crc32c::Unmask(DecodeFixed32(trailer + 1)) != crc) return Status::Corruption("block checksum mismatch");
  }
  if (trailer[0] != 0) return Status::Corruption("unknown block type");
  buf->resize(h.size);
  if (cache_ && ro.fill_cache) cache_->Insert(key, buf, buf->size());
  *out = std::move(buf);
  return Status::OK();
}

bool Table::KeyMayMatch(std::string_view internal_key) const {
  return filter_.empty() || BloomMayContain(filter_, ExtractUserKey(internal_key));
}

namespace {

// Walks the index block and opens data blocks on demand.
class TwoLevelIterator : public InternalIterator {
 public:
  TwoLevelIterator(const Table* t, const ReadOptions& ro, std::unique_ptr<InternalIterator> index)
      : table_(t), ro_(ro), index_(std::move(index)) {}

  bool Valid() const override { return data_ && data_->Valid(); }
  std::string_view key() const override { return data_->key(); }
  std::string_view value() const override { return data_->value(); }
  Status status() const override {
    if (!index_->status().ok()) return index_->status();
    if (!status_.ok()) return status_;
    return data_ ? data_->status() : Status::OK();
  }

  void SeekToFirst() override {
    index_->SeekToFirst();
    InitDataBlock();
    if (data_) data_->SeekToFirst();
    SkipEmpty();
  }
  void Seek(std::string_view target) override {
    index_->Seek(target);
    InitDataBlock();
    if (data_) data_->Seek(target);
    SkipEmpty();
  }
  void Next() override {
    data_->Next();
    SkipEmpty();
  }

 private:
  void InitDataBlock() {
    if (!index_->Valid()) {
      data_.reset();
      return;
    }
    BlockHandle h;
    std::string_view hv = index_->value();
    if (!h.DecodeFrom(&hv)) {
      status_ = Status::Corruption("bad block handle");
      data_.reset();
      return;
    }
    BlockContents block;
    Status s = table_->ReadBlock(ro_, h, &block);
    if (!s.ok()) {
      status_ = s;
      data_.reset();
      return;
    }
    data_ = NewBlockIterator(std::move(block));
  }

  void SkipEmpty() {
    while (data_ && !data_->Valid() && data_->status().ok()) {
      index_->Next();
      InitDataBlock();
      if (data_) data_->SeekToFirst();
    }
  }

  const Table* table_;
  ReadOptions ro_;
  std::unique_ptr<InternalIterator> index_;
  std::unique_ptr<InternalIterator> data_;
  Status status_;
};

}  // namespace

std::unique_ptr<InternalIterator> Table::NewIterator(const ReadOptions& ro) const {
  return std::make_unique<TwoLevelIterator>(this, ro, NewBlockIterator(index_));
}

}  // namespace lsmkv
