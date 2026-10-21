# Keep app binaries at build/<name>, as documented in the README
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

# Every app includes "apps/..." headers, so the repo root goes on the include path.
# Library targets bring "src/" themselves.

add_executable(book_dump book_dump/main.cpp)
target_include_directories(book_dump PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(book_dump PRIVATE ttt_feed) # ttt_feed brings ttt_core

add_executable(itch_replay itch_replay/main.cpp)
target_include_directories(itch_replay PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(itch_replay PRIVATE ttt_feed ttt_net)

add_executable(feed_rx feed_rx/main.cpp)
target_include_directories(feed_rx PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(feed_rx PRIVATE ttt_feed ttt_net)

add_executable(mock_exchange
  mock_exchange/main.cpp
  mock_exchange/exchange.cpp
)
target_include_directories(mock_exchange PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(mock_exchange PRIVATE ttt_gateway) # brings ttt_net, ttt_feed, ttt_core
