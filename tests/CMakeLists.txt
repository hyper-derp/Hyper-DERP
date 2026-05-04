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
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_frame_pool\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_policy\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_ice_agent\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_open_conn\\.cc")
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_einheit_channel\\.cc")
# test_hd_blake2s links its own copy of src/crypto/blake2s.cc
# instead of libderp; keep it out of the auto-glob.
list(FILTER UNIT_TEST_SOURCES
  EXCLUDE REGEX "test_hd_blake2s\\.cc")

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

# einheit control-channel integration test.
add_executable(test_einheit_channel
  test_einheit_channel.cc harness.cc)
target_link_libraries(test_einheit_channel PRIVATE
  libderp
  GTest::gtest_main
)
gtest_discover_tests(test_einheit_channel
  DISCOVERY_TIMEOUT 10
  PROPERTIES TIMEOUT 30
)

# Routing policy OpenConnection integration tests.
add_executable(test_open_conn test_open_conn.cc harness.cc)
target_link_libraries(test_open_conn PRIVATE
  libderp
  GTest::gtest_main
)
gtest_discover_tests(test_open_conn
  DISCOVERY_TIMEOUT 10
  PROPERTIES TIMEOUT 30
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
  hd_wg GTest::gtest_main)
gtest_discover_tests(test_wg_proxy
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

add_executable(test_wg_peer test_wg_peer.cc)
target_link_libraries(test_wg_peer PRIVATE
  hd_wg GTest::gtest_main)
gtest_discover_tests(test_wg_peer
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

add_executable(test_wg_config test_wg_config.cc)
target_link_libraries(test_wg_config PRIVATE
  hd_wg GTest::gtest_main)
gtest_discover_tests(test_wg_config
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

# Standalone Blake2s used for wg-relay MAC1.  Compiles its
# own object so we don't drag libderp's full link surface in.
add_executable(test_hd_blake2s
  test_hd_blake2s.cc
  ${PROJECT_SOURCE_DIR}/src/crypto/blake2s.cc)
target_include_directories(test_hd_blake2s PRIVATE
  ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_hd_blake2s PRIVATE
  GTest::gtest_main)
gtest_discover_tests(test_hd_blake2s
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 10)

# wg-relay diagnostic-surface smoke test (P0.1 / P0.2 in the
# 0.2.2 daemon brief). Drives the wg-relay forwarder
# in-process to confirm the trace path + per-peer counters
# are reachable.
add_executable(test_wg_relay_trace test_wg_relay_trace.cc)
target_link_libraries(test_wg_relay_trace PRIVATE
  libderp GTest::gtest_main)
gtest_discover_tests(test_wg_relay_trace
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

# wg-relay link-table regression tests (P0.3). Exercises the
# iter-1 invariant + the distinct WgRelayLinkAddResult codes
# the einheit channel maps onto error strings.
add_executable(test_wg_relay_links test_wg_relay_links.cc)
target_link_libraries(test_wg_relay_links PRIVATE
  libderp GTest::gtest_main)
gtest_discover_tests(test_wg_relay_links
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

# Frame pool unit test.
add_executable(test_frame_pool test_frame_pool.cc)
target_link_libraries(test_frame_pool PRIVATE
  hd_sdk GTest::gtest_main)
gtest_discover_tests(test_frame_pool
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

# Policy unit test (header-only, links sdk for config types).
add_executable(test_policy test_policy.cc)
target_link_libraries(test_policy PRIVATE
  hd_sdk GTest::gtest_main)
gtest_discover_tests(test_policy
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

# ICE agent test.
add_executable(test_ice_agent test_ice_agent.cc)
target_link_libraries(test_ice_agent PRIVATE
  hd_ice GTest::gtest_main)
gtest_discover_tests(test_ice_agent
  DISCOVERY_TIMEOUT 10 PROPERTIES TIMEOUT 30)

# hdwg end-to-end (netns-based, requires root + wireguard
# kernel module). Auto-skips otherwise via exit code 77.
add_test(
  NAME hd_wg_e2e
  COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/hd_wg_e2e.sh
    ${CMAKE_BINARY_DIR})
set_tests_properties(hd_wg_e2e PROPERTIES
  TIMEOUT 90
  SKIP_RETURN_CODE 77)

# wg-relay (`mode: wireguard`) end-to-end against the libvirt
# fleet hd-r2 / hd-c1 / hd-c2. SSH-driven, non-destructive — it
# expects the fleet to already be deployed per
# docs/wireguard_relay_quickstart.md and just exercises the
# operator workflow + asserts ping + counter movement. Skips
# (exit 77) on any host being unreachable.
add_test(
  NAME wg_relay_fleet
  COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/wg_relay_fleet.sh)
set_tests_properties(wg_relay_fleet PROPERTIES
  TIMEOUT 60
  SKIP_RETURN_CODE 77)
