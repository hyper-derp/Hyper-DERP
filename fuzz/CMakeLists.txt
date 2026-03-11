# Fuzz targets — requires Clang with libFuzzer support.

set(FUZZ_FLAGS "-fsanitize=fuzzer,address")

# -- fuzz_http ---------------------------------------------------------------
add_executable(fuzz_http fuzz_http.cc)
target_link_libraries(fuzz_http PRIVATE libderp)
target_compile_options(fuzz_http PRIVATE ${FUZZ_FLAGS})
target_link_options(fuzz_http PRIVATE ${FUZZ_FLAGS})

# -- fuzz_protocol -----------------------------------------------------------
add_executable(fuzz_protocol fuzz_protocol.cc)
target_link_libraries(fuzz_protocol PRIVATE libderp)
target_compile_options(fuzz_protocol PRIVATE ${FUZZ_FLAGS})
target_link_options(fuzz_protocol PRIVATE ${FUZZ_FLAGS})
