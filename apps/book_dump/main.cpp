#include "book_mode.hpp"
#include "core/mapped_file.hpp"
#include "options.hpp"
#include "stats.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
  const auto opts = book_dump::parse_args(argc, argv);
  if (!opts) {
    book_dump::usage(argv[0]);
    return 2;
  }
  try {
    const std::string path(opts->file);
    const ttt::core::MappedFile file(path);
    return opts->book_mode() ? book_dump::run_book(*opts, file)
                             : book_dump::run_stats(path.c_str(), file);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
