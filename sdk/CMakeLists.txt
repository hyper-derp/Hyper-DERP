# HD SDK core.
add_library(hd_sdk STATIC
  src/client.cc
  src/tunnel.cc
  src/frame_pool.cc)
target_include_directories(hd_sdk PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hd_sdk PUBLIC libderp)

# -- Extensions ---------------------------------------------------------------

# HdBridge: TCP/unix socket ↔ tunnel bridging.
add_library(hd_bridge STATIC
  libs/hd_bridge/bridge.cc)
target_include_directories(hd_bridge PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hd_bridge PUBLIC hd_sdk)

# HdIce: NAT traversal.
add_library(hd_ice STATIC
  libs/hd_ice/ice_agent.cc)
target_include_directories(hd_ice PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hd_ice PUBLIC hd_sdk)

# HdWg: WireGuard integration.
find_library(MNL_LIB mnl REQUIRED)
find_path(MNL_INCLUDE libmnl/libmnl.h REQUIRED)
add_library(hd_wg STATIC
  libs/hd_wg/wg_netlink.cc
  libs/hd_wg/wg_proxy.cc
  libs/hd_wg/wg_peer.cc
  libs/hd_wg/wg_config.cc)
target_include_directories(hd_wg PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include
  ${MNL_INCLUDE})
target_link_libraries(hd_wg PUBLIC
  hd_sdk hd_ice ${MNL_LIB})

# HdPolicy: header-only (policy.h).
# No library target needed.

# HdFleet: fleet topology view.
add_library(hd_fleet STATIC
  libs/hd_fleet/fleet_view.cc)
target_include_directories(hd_fleet PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hd_fleet PUBLIC hd_sdk)

# C ABI wrapper.
add_library(hd_sdk_c STATIC
  bindings/c/hd_sdk_c.cc)
target_include_directories(hd_sdk_c PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hd_sdk_c PUBLIC hd_sdk)
