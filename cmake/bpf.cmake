# BPF program compilation.
# Compiles bpf/hd_xdp.bpf.c -> build/bpf/hd_xdp.bpf.o
# using clang -target bpf.

find_program(CLANG_BPF clang REQUIRED)

set(BPF_SRC "${CMAKE_SOURCE_DIR}/bpf/hd_xdp.bpf.c")
set(BPF_OUT "${CMAKE_BINARY_DIR}/bpf/hd_xdp.bpf.o")

add_custom_command(
  OUTPUT ${BPF_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/bpf
  COMMAND ${CLANG_BPF} -O2 -g -target bpf
    -D__TARGET_ARCH_x86
    -I/usr/include/x86_64-linux-gnu
    -I${CMAKE_SOURCE_DIR}/bpf
    -c ${BPF_SRC}
    -o ${BPF_OUT}
  DEPENDS ${BPF_SRC}
  COMMENT "Compiling XDP program"
)

set(BPF_AF_XDP_SRC
  "${CMAKE_SOURCE_DIR}/bpf/hd_af_xdp.bpf.c")
set(BPF_AF_XDP_OUT
  "${CMAKE_BINARY_DIR}/bpf/hd_af_xdp.bpf.o")

add_custom_command(
  OUTPUT ${BPF_AF_XDP_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/bpf
  COMMAND ${CLANG_BPF} -O2 -g -target bpf
    -D__TARGET_ARCH_x86
    -I/usr/include/x86_64-linux-gnu
    -I${CMAKE_SOURCE_DIR}/bpf
    -c ${BPF_AF_XDP_SRC}
    -o ${BPF_AF_XDP_OUT}
  DEPENDS ${BPF_AF_XDP_SRC}
  COMMENT "Compiling AF_XDP redirect program"
)

set(BPF_WG_RELAY_SRC
  "${CMAKE_SOURCE_DIR}/bpf/wg_relay.bpf.c")
set(BPF_WG_RELAY_OUT
  "${CMAKE_BINARY_DIR}/bpf/wg_relay.bpf.o")

add_custom_command(
  OUTPUT ${BPF_WG_RELAY_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/bpf
  COMMAND ${CLANG_BPF} -O2 -g -target bpf
    -D__TARGET_ARCH_x86
    -I/usr/include/x86_64-linux-gnu
    -I${CMAKE_SOURCE_DIR}/bpf
    -c ${BPF_WG_RELAY_SRC}
    -o ${BPF_WG_RELAY_OUT}
  DEPENDS ${BPF_WG_RELAY_SRC}
  COMMENT "Compiling wg-relay XDP program"
)

add_custom_target(xdp_program ALL
  DEPENDS ${BPF_OUT} ${BPF_AF_XDP_OUT} ${BPF_WG_RELAY_OUT}
)
