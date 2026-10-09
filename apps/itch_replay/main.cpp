#include "core/mapped_file.hpp"
#include "options.hpp"
#include "replayer.hpp"

#include <string>

int main(int argc, char** argv) {
  const auto opts = itch_replay::parse_args(argc, argv);
  if (!opts) {
    itch_replay::usage(argv[0]);
    return 2;
  }

  try {
    const ttt::core::MappedFile file(std::string(opts->file));
    itch_replay::Replayer(file.bytes(), *opts).run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
