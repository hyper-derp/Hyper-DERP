add_library(hd_sdk STATIC
  src/client.cc
  src/tunnel.cc
  src/frame_pool.cc)
target_include_directories(hd_sdk PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hd_sdk PUBLIC libderp)

# Extensions.
add_library(hd_bridge STATIC
  libs/hd_bridge/bridge.cc)
target_include_directories(hd_bridge PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hd_bridge PUBLIC hd_sdk)
