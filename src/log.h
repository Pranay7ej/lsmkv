// Record log used for both the WAL and the manifest.
//
// record := masked_crc32c(len + payload) u32 | len u32 | payload
//
// A crash can leave a partial record at the end. The reader treats a record
// that runs past the end of the file, or whose checksum is wrong, as the end
// of the log, and reports how many bytes it had to drop.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "file.h"

namespace lsmkv {

class LogWriter {
 public:
  explicit LogWriter(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}
  Status AddRecord(std::string_view payload);
  Status Sync() { return file_->Sync(); }
  Status Flush() { return file_->Flush(); }
  Status Close() { return file_->Close(); }
  uint64_t size() const { return file_->size(); }

 private:
  std::unique_ptr<WritableFile> file_;
};

class LogReader {
 public:
  // Reads the whole file up front; logs here are at most a few MB.
  static Status Open(const std::string& path, std::unique_ptr<LogReader>* out);
  explicit LogReader(std::string contents) : data_(std::move(contents)) {}

  bool ReadRecord(std::string_view* record);
  // Bytes after the last good record (torn write or corruption).
  size_t dropped_bytes() const { return dropped_; }
  bool hit_bad_checksum() const { return bad_checksum_; }

 private:
  std::string data_;
  size_t pos_ = 0;
  size_t dropped_ = 0;
  bool bad_checksum_ = false;
};

}  // namespace lsmkv
