find_library(MNL_LIB mnl REQUIRED)
find_path(MNL_INCLUDE libmnl/libmnl.h REQUIRED)

add_library(wg_client STATIC
  wg_netlink.cc
  wg_proxy.cc
  wg_peer.cc)
target_include_directories(wg_client PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include
  ${MNL_INCLUDE})
target_link_libraries(wg_client PUBLIC
  libderp ${MNL_LIB} spdlog::spdlog)

add_executable(hd-wg hd_wg.cc)
target_link_libraries(hd-wg PRIVATE wg_client)

add_executable(hdctl hdctl.cc)
target_link_libraries(hdctl PRIVATE libderp ${ZMQ_LIB})
target_include_directories(hdctl PRIVATE ${ZMQ_INCLUDE_DIR})
