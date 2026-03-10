# libderp — core DERP relay library.

find_library(SODIUM_LIB sodium REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
message(STATUS "libsodium: ${SODIUM_LIB}")

add_library(libderp_obj OBJECT
  src/protocol.cc
  src/data_plane.cc
  src/http.cc
  src/handshake.cc
  src/server.cc
)
target_include_directories(libderp_obj PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${URING_INCLUDE_DIR}
  ${SODIUM_INCLUDE_DIR}
)
target_link_libraries(libderp_obj PUBLIC
  ${URING_LIB}
  ${SODIUM_LIB}
  spdlog::spdlog
  pthread
)
target_compile_definitions(libderp_obj PUBLIC
  HAVE_IO_URING
)

add_library(libderp STATIC $<TARGET_OBJECTS:libderp_obj>)
target_include_directories(libderp PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${URING_INCLUDE_DIR}
  ${SODIUM_INCLUDE_DIR}
)
target_link_libraries(libderp PUBLIC
  ${URING_LIB}
  ${SODIUM_LIB}
  spdlog::spdlog
  pthread
)
target_compile_definitions(libderp PUBLIC
  HAVE_IO_URING
)
