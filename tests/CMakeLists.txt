include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(unit_tests
  unit/async_logger_test.cpp
  unit/book_manager_test.cpp
  unit/core_platform_test.cpp
  unit/feed_handler_test.cpp
  unit/moldudp64_test.cpp
  unit/order_book_test.cpp
  unit/order_map_test.cpp
  unit/order_pool_test.cpp
  unit/order_state_test.cpp
  unit/ouch_codec_test.cpp
  unit/price_level_test.cpp
  unit/soupbintcp_test.cpp
  unit/spsc_queue_test.cpp
)
target_include_directories(unit_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} # "utils/..."
  ${PROJECT_SOURCE_DIR}       # "apps/mock_exchange/ouch_encode.hpp"
)
target_link_libraries(unit_tests PRIVATE ttt_gateway GTest::gtest_main) # brings ttt_feed
gtest_discover_tests(unit_tests PROPERTIES LABELS unit)

# Real loopback TCP against an in-process mock exchange
add_executable(integration_tests
  integration/gateway_session_test.cpp
  ${PROJECT_SOURCE_DIR}/apps/mock_exchange/exchange.cpp
)
target_include_directories(integration_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(integration_tests PRIVATE ttt_gateway GTest::gtest_main)
gtest_discover_tests(integration_tests PROPERTIES LABELS integration)
