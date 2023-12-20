// Thin POSIX file wrappers.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/status.h"

namespace lsmkv {

class WritableFile {
 public:
  ~WritableFile();
  static Status Open(const std::string& path, std::unique_ptr<WritableFile>* out);
  Status Append(std::string_view data);
  Status Flush();  // hand buffered bytes to the kernel
  Status Sync();   // flush + fsync
  Status Close();
  uint64_t size() const { return size_; }

 private:
  WritableFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  int fd_;
  std::string path_;
  std::string buf_;
  uint64_t size_ = 0;
};

class RandomAccessFile {
 public:
  ~RandomAccessFile();
  static Status Open(const std::string& path, std::unique_ptr<RandomAccessFile>* out);
  // Reads exactly n bytes at offset into scratch; returns Corruption on a short read.
  Status Read(uint64_t offset, size_t n, std::string* scratch) const;
  uint64_t size() const { return size_; }

 private:
  RandomAccessFile(int fd, uint64_t size, std::string path) : fd_(fd), size_(size), path_(std::move(path)) {}
  int fd_;
  uint64_t size_;
  std::string path_;
};

Status ReadFileToString(const std::string& path, std::string* out);
Status WriteStringToFileSync(const std::string& path, std::string_view data);
bool FileExists(const std::string& path);
Status GetChildren(const std::string& dir, std::vector<std::string>* names);
Status RemoveFile(const std::string& path);
Status RenameFile(const std::string& from, const std::string& to);
Status CreateDirIfMissing(const std::string& dir);
Status SyncDir(const std::string& dir);
Status GetFileSize(const std::string& path, uint64_t* size);

// Advisory lock so two processes can't open the same DB.
class FileLock {
 public:
  ~FileLock();
  static Status Acquire(const std::string& path, std::unique_ptr<FileLock>* out);

 private:
  explicit FileLock(int fd) : fd_(fd) {}
  int fd_;
};

}  // namespace lsmkv
