include(GoogleTest)

# Unit tests (auto-discover, exclude e2e and harness).
file(GLOB UNIT_TEST_SOURCES
  CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_(hd_)?e2e\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_production\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_wg_.*\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_ctl_channel\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_hd_peerinfo\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_hd_reconnect\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_hd_sdk\\.cc")

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

# WG client tests (need client/ include path).
add_executable(test_wg_proxy test_wg_proxy.cc)
target_link_libraries(test_wg_proxy PRIVATE
  wg_client GTest::gtest_main)
gtest_discover_tests(test_wg_proxy
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

add_executable(test_wg_peer test_wg_peer.cc)
target_link_libraries(test_wg_peer PRIVATE
  wg_client GTest::gtest_main)
gtest_discover_tests(test_wg_peer
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

add_executable(test_wg_config test_wg_config.cc)
target_link_libraries(test_wg_config PRIVATE
  wg_client GTest::gtest_main)
gtest_discover_tests(test_wg_config
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

# ZMQ control channel tests.
add_executable(test_ctl_channel test_ctl_channel.cc)
target_link_libraries(test_ctl_channel PRIVATE
  libderp GTest::gtest_main)
gtest_discover_tests(test_ctl_channel
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

# HD PeerInfo + MeshData e2e.
add_executable(test_hd_peerinfo test_hd_peerinfo.cc harness.cc)
target_link_libraries(test_hd_peerinfo PRIVATE
  libderp GTest::gtest_main)
gtest_discover_tests(test_hd_peerinfo
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 60)

# HD reconnect e2e.
add_executable(test_hd_reconnect test_hd_reconnect.cc harness.cc)
target_link_libraries(test_hd_reconnect PRIVATE
  libderp GTest::gtest_main)
gtest_discover_tests(test_hd_reconnect
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 60)

# HD SDK e2e.
add_executable(test_hd_sdk test_hd_sdk.cc harness.cc)
target_link_libraries(test_hd_sdk PRIVATE
  hd_sdk GTest::gtest_main)
gtest_discover_tests(test_hd_sdk
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 60)
