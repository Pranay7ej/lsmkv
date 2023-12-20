#pragma once

#include <string>
#include <string_view>

namespace lsmkv {

class Status {
 public:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status() = default;
  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg = {}) { return Status(Code::kNotFound, msg); }
  static Status Corruption(std::string_view msg) { return Status(Code::kCorruption, msg); }
  static Status IOError(std::string_view msg) { return Status(Code::kIOError, msg); }
  static Status InvalidArgument(std::string_view msg) { return Status(Code::kInvalidArgument, msg); }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  Code code() const { return code_; }

  std::string ToString() const {
    const char* names[] = {"OK", "NotFound", "Corruption", "IOError", "InvalidArgument"};
    std::string s = names[static_cast<int>(code_)];
    if (!msg_.empty()) s += ": " + msg_;
    return s;
  }

 private:
  Status(Code c, std::string_view msg) : code_(c), msg_(msg) {}
  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace lsmkv
