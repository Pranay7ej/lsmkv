#include "log.h"

#include "coding.h"
#include "crc32c.h"

namespace lsmkv {

Status LogWriter::AddRecord(std::string_view payload) {
  char header[8];
  EncodeFixed32(header + 4, static_cast<uint32_t>(payload.size()));
  uint32_t crc = crc32c::Value(header + 4, 4);
  crc = crc32c::Extend(crc, payload.data(), payload.size());
  EncodeFixed32(header, crc32c::Mask(crc));
  Status s = file_->Append(std::string_view(header, 8));
  if (s.ok()) s = file_->Append(payload);
  return s;
}

Status LogReader::Open(const std::string& path, std::unique_ptr<LogReader>* out) {
  std::string contents;
  Status s = ReadFileToString(path, &contents);
  if (!s.ok()) return s;
  *out = std::make_unique<LogReader>(std::move(contents));
  return Status::OK();
}

bool LogReader::ReadRecord(std::string_view* record) {
  if (pos_ >= data_.size()) return false;
  const size_t left = data_.size() - pos_;
  auto stop = [&] {
    dropped_ = data_.size() - pos_;
    pos_ = data_.size();
    return false;
  };
  if (left < 8) return stop();
  const char* p = data_.data() + pos_;
  const uint32_t len = DecodeFixed32(p + 4);
  if (len > left - 8) return stop();
  uint32_t crc = crc32c::Value(p + 4, 4);
  crc = crc32c::Extend(crc, p + 8, len);
  if (crc32c::Unmask(DecodeFixed32(p)) != crc) {
    bad_checksum_ = true;
    return stop();
  }
  *record = std::string_view(p + 8, len);
  pos_ += 8 + len;
  return true;
}

}  // namespace lsmkv
