#include "file.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace lsmkv {
namespace {

Status PosixError(const std::string& ctx, int err) { return Status::IOError(ctx + ": " + std::strerror(err)); }

constexpr size_t kWriteBuffer = 64 * 1024;

}  // namespace

WritableFile::~WritableFile() {
  if (fd_ >= 0) Close();
}

Status WritableFile::Open(const std::string& path, std::unique_ptr<WritableFile>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return PosixError(path, errno);
  out->reset(new WritableFile(fd, path));
  return Status::OK();
}

Status WritableFile::Append(std::string_view data) {
  size_ += data.size();
  if (buf_.size() + data.size() < kWriteBuffer) {
    buf_.append(data.data(), data.size());
    return Status::OK();
  }
  Status s = Flush();
  if (!s.ok()) return s;
  if (data.size() < kWriteBuffer) {
    buf_.append(data.data(), data.size());
    return Status::OK();
  }
  const char* p = data.data();
  size_t n = data.size();
  while (n > 0) {
    ssize_t w = ::write(fd_, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_, errno);
    }
    p += w;
    n -= size_t(w);
  }
  return Status::OK();
}

Status WritableFile::Flush() {
  const char* p = buf_.data();
  size_t n = buf_.size();
  while (n > 0) {
    ssize_t w = ::write(fd_, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_, errno);
    }
    p += w;
    n -= size_t(w);
  }
  buf_.clear();
  return Status::OK();
}

Status WritableFile::Sync() {
  Status s = Flush();
  if (!s.ok()) return s;
  if (::fdatasync(fd_) != 0) return PosixError(path_, errno);
  return Status::OK();
}

Status WritableFile::Close() {
  Status s = Flush();
  if (fd_ >= 0 && ::close(fd_) != 0 && s.ok()) s = PosixError(path_, errno);
  fd_ = -1;
  return s;
}

RandomAccessFile::~RandomAccessFile() {
  if (fd_ >= 0) ::close(fd_);
}

Status RandomAccessFile::Open(const std::string& path, std::unique_ptr<RandomAccessFile>* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return PosixError(path, errno);
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    int e = errno;
    ::close(fd);
    return PosixError(path, e);
  }
  out->reset(new RandomAccessFile(fd, uint64_t(st.st_size), path));
  return Status::OK();
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, std::string* scratch) const {
  scratch->resize(n);
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd_, scratch->data() + done, n - done, off_t(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_, errno);
    }
    if (r == 0) return Status::Corruption(path_ + ": short read");
    done += size_t(r);
  }
  return Status::OK();
}

Status ReadFileToString(const std::string& path, std::string* out) {
  std::unique_ptr<RandomAccessFile> f;
  Status s = RandomAccessFile::Open(path, &f);
  if (!s.ok()) return s;
  return f->Read(0, f->size(), out);
}

Status WriteStringToFileSync(const std::string& path, std::string_view data) {
  std::unique_ptr<WritableFile> f;
  Status s = WritableFile::Open(path, &f);
  if (s.ok()) s = f->Append(data);
  if (s.ok()) s = f->Sync();
  if (s.ok()) s = f->Close();
  if (!s.ok()) ::unlink(path.c_str());
  return s;
}

bool FileExists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

Status GetChildren(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (!d) return PosixError(dir, errno);
  while (struct dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names->emplace_back(e->d_name);
  }
  ::closedir(d);
  return Status::OK();
}

Status RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0) return PosixError(path, errno);
  return Status::OK();
}

Status RenameFile(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) return PosixError(from, errno);
  return Status::OK();
}

Status CreateDirIfMissing(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return PosixError(dir, errno);
  return Status::OK();
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return PosixError(dir, errno);
  Status s;
  if (::fsync(fd) != 0) s = PosixError(dir, errno);
  ::close(fd);
  return s;
}

Status GetFileSize(const std::string& path, uint64_t* size) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return PosixError(path, errno);
  *size = uint64_t(st.st_size);
  return Status::OK();
}

FileLock::~FileLock() {
  ::flock(fd_, LOCK_UN);
  ::close(fd_);
}

Status FileLock::Acquire(const std::string& path, std::unique_ptr<FileLock>* out) {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return PosixError(path, errno);
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int e = errno;
    ::close(fd);
    return Status::IOError(path + ": already locked by another process (" + std::strerror(e) + ")");
  }
  out->reset(new FileLock(fd));
  return Status::OK();
}

}  // namespace lsmkv
