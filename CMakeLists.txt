cmake_minimum_required(VERSION 3.25)
project(tick_to_trade CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(ttt_core STATIC
  src/core/mapped_file.cpp
  src/core/async_logger.cpp
)
target_include_directories(ttt_core PUBLIC src)
target_compile_options(ttt_core PUBLIC -Wall -Wextra -Wpedantic)

add_library(ttt_feed STATIC
  src/feed/book/book_manager.cpp
  src/feed/book/order_book.cpp
)
target_link_libraries(ttt_feed PUBLIC ttt_core)

add_library(ttt_net STATIC
  src/net/udp_socket.cpp
  src/net/tcp_client.cpp
)
target_link_libraries(ttt_net PUBLIC ttt_core)

add_library(ttt_gateway STATIC
  src/gateway/common/order_state.cpp
  src/gateway/soupbintcp/session.cpp
)
target_link_libraries(ttt_gateway PUBLIC ttt_net ttt_feed)

# --- apps -----------------------------------------------------------------------------

# After the libraries: the apps link against them
add_subdirectory(apps)

# --- tests ----------------------------------------------------------------------------

# Defines BUILD_TESTING (ON by default) and calls enable_testing(), so ctest run
# from the build root finds the tests. Configure with -DBUILD_TESTING=OFF to skip.
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# --- benchmarks -----------------------------------------------------------------------

# Only meaningful in Release builds. Configure with -DBUILD_BENCHMARKS=OFF to skip.
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
