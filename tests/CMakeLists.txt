include(GoogleTest)

# Unit tests (auto-discover, exclude e2e and harness).
file(GLOB UNIT_TEST_SOURCES
  CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_(hd_)?e2e\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_production\\.cc")

foreach(test_source ${UNIT_TEST_SOURCES})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE
    libderp
    GTest::gtest_main
  )
  gtest_discover_tests(${test_name}
    DISCOVERY_TIMEOUT 10
    PROPERTIES TIMEOUT 30
  )
endforeach()

# Integration tests (e2e with harness).
add_executable(test_e2e test_e2e.cc harness.cc)
target_link_libraries(test_e2e PRIVATE
  libderp
  GTest::gtest_main
)
gtest_discover_tests(test_e2e
  DISCOVERY_TIMEOUT 10
  PROPERTIES TIMEOUT 60
)

# HD protocol e2e tests.
add_executable(test_hd_e2e test_hd_e2e.cc harness.cc)
target_link_libraries(test_hd_e2e PRIVATE
  libderp
  GTest::gtest_main
)
gtest_discover_tests(test_hd_e2e
  DISCOVERY_TIMEOUT 10
  PROPERTIES TIMEOUT 60
)

# Production hardening tests (uses harness).
add_executable(test_production test_production.cc harness.cc)
target_link_libraries(test_production PRIVATE
  libderp
  GTest::gtest_main
)
gtest_discover_tests(test_production
  DISCOVERY_TIMEOUT 10
  PROPERTIES TIMEOUT 60
)
