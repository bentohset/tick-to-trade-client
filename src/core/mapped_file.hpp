#pragma once

#include <filesystem>
#include <span>

namespace ttt::core {

// RAII mmap of a whole file as a byte span
class MappedFile {
public:
  explicit MappedFile(const std::filesystem::path&); // open, fstat, mmap
  ~MappedFile();
  MappedFile(MappedFile&&) noexcept;
  MappedFile& operator=(MappedFile&&) noexcept; // assign

  std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

private:
  const std::byte* data_{nullptr};
  std::size_t size_{0};
};

} // namespace ttt::core
