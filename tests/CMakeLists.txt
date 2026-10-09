include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(unit_tests
  unit/book_manager_test.cpp
  unit/moldudp64_gap_test.cpp
  unit/order_book_test.cpp
  unit/order_map_test.cpp
  unit/order_pool_test.cpp
  unit/price_level_test.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}) # "utils/..."
target_link_libraries(unit_tests PRIVATE ttt_feed GTest::gtest_main)
gtest_discover_tests(unit_tests)
