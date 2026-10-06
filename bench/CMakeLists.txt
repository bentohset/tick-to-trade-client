include(FetchContent)

# Only the library: skip Google Benchmark's own tests (and their GoogleTest dependency)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(bench_itch_parse bench_itch_parse.cpp)
target_link_libraries(bench_itch_parse PRIVATE ttt_core benchmark::benchmark)
