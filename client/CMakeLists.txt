find_library(MNL_LIB mnl REQUIRED)
find_path(MNL_INCLUDE libmnl/libmnl.h REQUIRED)

add_library(wg_netlink STATIC wg_netlink.cc)
target_include_directories(wg_netlink PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include
  ${MNL_INCLUDE})
target_link_libraries(wg_netlink PUBLIC
  ${MNL_LIB} spdlog::spdlog)
