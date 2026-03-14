cmake_minimum_required(VERSION 3.20...4.20)

# ============================================================================
# Compiler Selection — MUST be before project()
# ============================================================================
if(NOT CMAKE_CROSSCOMPILING
    AND NOT DEFINED CMAKE_C_COMPILER
    AND NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(_clang clang)
  find_program(_clangpp clang++)
  if(_clang AND _clangpp)
    set(CMAKE_C_COMPILER "${_clang}"
      CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${_clangpp}"
      CACHE FILEPATH "C++ compiler" FORCE)
  endif()
  unset(_clang CACHE)
  unset(_clangpp CACHE)
endif()

project(hyper-derp
  LANGUAGES C CXX
  DESCRIPTION "High-performance DERP relay server"
  VERSION 0.1.0
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_PARALLEL_LEVEL)
  set(CMAKE_BUILD_PARALLEL_LEVEL ""
    CACHE STRING
    "The maximum number of concurrent build processes."
    "Leave empty for no limit.")
endif()

# Detect and select fastest available ELF linker.
if(UNIX)
  set(_found_linker FALSE)
  foreach(_ld mold lld gold)
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C which ${_ld}
      OUTPUT_VARIABLE _path
      RESULT_VARIABLE _res
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(_res EQUAL 0 AND _path)
      execute_process(
        COMMAND "${_path}" --version
        RESULT_VARIABLE _ld_works
        OUTPUT_QUIET ERROR_QUIET
      )
      if(_ld_works EQUAL 0)
        message(STATUS "Using linker: ${_ld} at ${_path}")
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=${_ld}")
        set(CMAKE_SHARED_LINKER_FLAGS
            "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=${_ld}")
        set(CMAKE_MODULE_LINKER_FLAGS
            "${CMAKE_MODULE_LINKER_FLAGS} -fuse-ld=${_ld}")
        set(_found_linker TRUE)
        break()
      endif()
    endif()
  endforeach()

  if(NOT _found_linker)
    message(STATUS
      "No mold/lld/gold found; using default ld with "
      "-Wl,--start-group")
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -Wl,--start-group")
  endif()
endif()

# Debug-specific flags.
if(CMAKE_BUILD_TYPE MATCHES Debug)
  message(STATUS "Debug mode: adding debug-specific flags.")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS_DEBUG
        "${CMAKE_C_FLAGS_DEBUG} -O0 -g -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS_DEBUG
        "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g -fno-omit-frame-pointer")

    find_program(LLDB_EXECUTABLE lldb)
    if(LLDB_EXECUTABLE)
      set(CMAKE_C_FLAGS_DEBUG
          "${CMAKE_C_FLAGS_DEBUG} -glldb")
      set(CMAKE_CXX_FLAGS_DEBUG
          "${CMAKE_CXX_FLAGS_DEBUG} -glldb")
      message(STATUS "LLDB detected. Adding -glldb.")
    endif()
  endif()
endif()

message(STATUS "Build system: ${CMAKE_GENERATOR}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")

# Find system liburing.
find_library(URING_LIB uring REQUIRED)
find_path(URING_INCLUDE_DIR liburing.h REQUIRED)
message(STATUS "liburing: ${URING_LIB}")

# Fetch 3rd party libraries.
include("cmake/3rd_party_libs/fetch.cmake")

# Build libderp.
include("cmake/libderp.cmake")

# Main executable.
add_executable(hyper-derp src/main.cc)
target_link_libraries(hyper-derp PRIVATE libderp)

# Tools.
add_subdirectory(tools)

# Tests.
enable_testing()
add_subdirectory(tests)

include(CTest)

# Fuzz targets (only with Clang + libFuzzer).
option(ENABLE_FUZZING "Build fuzz targets" OFF)
if(ENABLE_FUZZING)
  add_subdirectory(fuzz)
endif()

# ============================================================================
# Install rules
# ============================================================================
install(TARGETS hyper-derp derp-test-client derp-scale-test derp-tun-proxy
  RUNTIME DESTINATION bin)
install(FILES
  ${CMAKE_SOURCE_DIR}/dist/hyper-derp.service
  DESTINATION lib/systemd/system)
install(FILES
  ${CMAKE_SOURCE_DIR}/dist/hyper-derp.conf
  DESTINATION /etc/hyper-derp
  RENAME hyper-derp.conf.example)

# ============================================================================
# CPack — Debian package
# ============================================================================
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "hyper-derp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_DESCRIPTION
  "Tailscale-compatible DERP relay server built with C++23 and io_uring for high-throughput, low-latency packet relay.")
set(CPACK_PACKAGE_CONTACT
  "karl <karl@localhost>")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "liburing2 (>= 2.9), libsodium23 (>= 1.0.18), libspdlog1.15 (>= 1:1.15)")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE
  "https://github.com/karl/hyper-derp")
# Let dpkg-architecture determine the correct name.
execute_process(
  COMMAND dpkg --print-architecture
  OUTPUT_VARIABLE _dpkg_arch
  OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_dpkg_arch}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# Post-install/remove scripts for systemd integration.
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
  "${CMAKE_SOURCE_DIR}/dist/postinst;${CMAKE_SOURCE_DIR}/dist/prerm;${CMAKE_SOURCE_DIR}/dist/postrm")

include(CPack)
