#include "core/mapped_file.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ttt::core {

namespace {

[[noreturn]] void throw_errno(const char* what, const std::filesystem::path& path) {
  throw std::system_error(errno, std::generic_category(), std::string(what) + " " + path.string());
}

// closes the fd with RAII
struct FdGuard {
  int fd;
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
};

} // namespace

MappedFile::MappedFile(const std::filesystem::path& path) {
  FdGuard fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (fd.fd < 0) throw_errno("open", path);

  struct stat st{};
  if (::fstat(fd.fd, &st) != 0) throw_errno("fstat", path);
  if (!S_ISREG(st.st_mode)) {
    throw std::system_error(EINVAL, std::generic_category(),
                            "not a regular file: " + path.string());
  }

  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ == 0) return;

  int flags = MAP_PRIVATE;
#ifdef MAP_POPULATE
  flags |= MAP_POPULATE; // Linux: prefault every page now, not during parsing
#endif
  void* p = ::mmap(nullptr, size_, PROT_READ, flags, fd.fd, 0);
  if (p == MAP_FAILED) throw_errno("mmap", path);

  ::madvise(p, size_, MADV_SEQUENTIAL); // hint
  data_ = static_cast<const std::byte*>(p);

  // fd closes by RAII; mapping stays valid
}

MappedFile::~MappedFile() {
  if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

} // namespace ttt::core
