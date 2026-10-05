cmake_minimum_required(VERSION 3.25)
project(tick_to_trade CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(ttt_core STATIC
  src/core/mapped_file.cpp
)
target_include_directories(ttt_core PUBLIC src)
target_compile_options(ttt_core PUBLIC -Wall -Wextra -Wpedantic)

add_executable(book_dump apps/book_dump/main.cpp)
target_link_libraries(book_dump PRIVATE ttt_core)
