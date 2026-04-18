add_library(hd_sdk STATIC hd_sdk.cc)
target_include_directories(hd_sdk PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hd_sdk PUBLIC libderp)
