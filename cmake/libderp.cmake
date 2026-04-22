# libderp — core DERP relay library.

find_library(SODIUM_LIB sodium REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
message(STATUS "libsodium: ${SODIUM_LIB}")

find_package(OpenSSL REQUIRED)

find_library(BPF_LIB bpf REQUIRED)
find_path(BPF_INCLUDE_DIR bpf/libbpf.h REQUIRED)
message(STATUS "libbpf: ${BPF_LIB}")

find_library(ZMQ_LIB zmq REQUIRED)
find_path(ZMQ_INCLUDE_DIR zmq.hpp REQUIRED)
message(STATUS "libzmq: ${ZMQ_LIB}")

add_library(libderp_obj OBJECT
  src/protocol.cc
  src/hd_protocol.cc
  src/data_plane.cc
  src/http.cc
  src/handshake.cc
  src/server.cc
  src/client.cc
  src/bench.cc
  src/control_plane.cc
  src/tun.cc
  src/metrics.cc
  src/ktls.cc
  src/config.cc
  src/hd_peers.cc
  src/hd_handshake.cc
  src/hd_client.cc
  src/hd_bridge.cc
  src/hd_relay_table.cc
  src/stun.cc
  src/turn.cc
  src/xdp_loader.cc
  src/ice.cc
  src/ctl_channel.cc
  src/key_format.cc
  src/hd_resolver.cc
  src/hd_audit.cc
  src/fleet_controller.cc
  src/einheit_protocol.cc
)
target_include_directories(libderp_obj PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${URING_INCLUDE_DIR}
  ${SODIUM_INCLUDE_DIR}
  ${BPF_INCLUDE_DIR}
  ${ZMQ_INCLUDE_DIR}
)
target_link_libraries(libderp_obj PUBLIC
  ${URING_LIB}
  ${SODIUM_LIB}
  ${BPF_LIB}
  ${ZMQ_LIB}
  spdlog::spdlog
  Crow::Crow
  OpenSSL::SSL
  OpenSSL::Crypto
  ryml::ryml
  msgpack-cxx
  pthread
)
target_compile_definitions(libderp_obj PUBLIC
  HAVE_IO_URING
  CROW_ENABLE_SSL
)

add_library(libderp STATIC $<TARGET_OBJECTS:libderp_obj>)
target_include_directories(libderp PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${URING_INCLUDE_DIR}
  ${SODIUM_INCLUDE_DIR}
  ${BPF_INCLUDE_DIR}
)
target_link_libraries(libderp PUBLIC
  ${URING_LIB}
  ${SODIUM_LIB}
  ${BPF_LIB}
  ${ZMQ_LIB}
  spdlog::spdlog
  Crow::Crow
  OpenSSL::SSL
  OpenSSL::Crypto
  ryml::ryml
  msgpack-cxx
  pthread
)
target_compile_definitions(libderp PUBLIC
  HAVE_IO_URING
  CROW_ENABLE_SSL
)
