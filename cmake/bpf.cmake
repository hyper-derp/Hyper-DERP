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

add_custom_target(xdp_program ALL
  DEPENDS ${BPF_OUT}
)
