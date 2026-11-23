include(GoogleTest)

add_executable(orderflow_tests
  test_endian.cpp
  test_itch.cpp
  test_price_levels.cpp
  test_order_map.cpp
  test_book.cpp
  test_l2_book.cpp
  test_perf_counters.cpp
  test_backtest.cpp
  test_itch_extract.cpp
  test_spsc.cpp
  test_pool.cpp
)
target_link_libraries(orderflow_tests PRIVATE orderflow orderflow_warnings GTest::gtest_main)
gtest_discover_tests(orderflow_tests)
