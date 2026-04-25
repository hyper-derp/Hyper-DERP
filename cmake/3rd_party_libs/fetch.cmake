# Required system packages:
#   libgtest-dev libgmock-dev libcli11-dev
#   libssl-dev libasio-dev

if(BUILD_TESTING OR NOT DEFINED BUILD_TESTING)
  find_package(GTest REQUIRED)
endif()
find_package(CLI11 REQUIRED)

# ---- FetchContent dependencies ----------------------------------------------

include(FetchContent)

# spdlog — statically linked with bundled fmt to avoid
# distro soname mismatches (Ubuntu vs Debian).
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# msgpack-cxx — MessagePack codec used by the einheit control
# channel. Keep the version aligned with einheit-cli; both sides
# share the same wire format, and version drift here would silently
# corrupt Request / Response envelopes.
FetchContent_Declare(msgpack-cxx
  GIT_REPOSITORY https://github.com/msgpack/msgpack-c.git
  GIT_TAG cpp-6.1.1
  GIT_SHALLOW TRUE
)
set(MSGPACK_CXX20 OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(msgpack-cxx)

# rapidyaml — fast YAML parser.
FetchContent_Declare(ryml
  GIT_REPOSITORY https://github.com/biojppm/rapidyaml.git
  GIT_TAG v0.8.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ryml)

# Crow — lightweight C++ HTTP framework.
set(CROW_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(CROW_FEATURES "ssl" CACHE STRING "" FORCE)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG v1.3.0.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(crow)

if(TARGET Crow::Crow)
  # OK
elseif(TARGET Crow)
  add_library(Crow::Crow ALIAS Crow)
else()
  message(FATAL_ERROR "Crow target missing.")
endif()

# einheit-cli — operator CLI shipped inside the hyper-derp deb so
# `apt install hyper-derp` is fully functional out of the box. The
# einheit sub-build emits a `/usr/bin/einheit` install target; hyper-
# derp's `hd-cli` wrapper resolves it via $PATH.
#
# Source resolution priority:
#   1. EINHEIT_SOURCE_DIR cmake var or env (sibling source tree)
#   2. ../einheit/cli relative to the Hyper-DERP repo root
#   3. FetchContent from einheitdev/cli (ssh; needs access)
#
# Build-time only; not on the runtime data path.
set(_einheit_local "")
if(DEFINED EINHEIT_SOURCE_DIR AND EXISTS "${EINHEIT_SOURCE_DIR}/CMakeLists.txt")
  set(_einheit_local "${EINHEIT_SOURCE_DIR}")
elseif(DEFINED ENV{EINHEIT_SOURCE_DIR}
       AND EXISTS "$ENV{EINHEIT_SOURCE_DIR}/CMakeLists.txt")
  set(_einheit_local "$ENV{EINHEIT_SOURCE_DIR}")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/../einheit/cli/CMakeLists.txt")
  set(_einheit_local "${PROJECT_SOURCE_DIR}/../einheit/cli")
endif()

set(EINHEIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EINHEIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING_einheit OFF CACHE BOOL "" FORCE)
# Don't ship einheit's headers / static archive in the hyper-derp
# deb; we only consume the runtime binary.
set(EINHEIT_INSTALL_DEVEL OFF CACHE BOOL "" FORCE)

# We deliberately do NOT pass EXCLUDE_FROM_ALL to add_subdirectory
# — that flag also disables install() rules in the subdirectory,
# which would silently drop the einheit binary out of the deb.
# With tests/examples already gated off above, the only outputs
# pulled in are the framework libs + adapter + binary, which is
# what we want.
if(_einheit_local)
  message(STATUS "einheit: using local source at ${_einheit_local}")
  add_subdirectory("${_einheit_local}"
    "${CMAKE_BINARY_DIR}/_deps/einheit-build")
else()
  message(STATUS "einheit: fetching via FetchContent")
  FetchContent_Declare(einheit-cli
    GIT_REPOSITORY git@github.com:einheitdev/cli.git
    GIT_TAG main
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(einheit-cli)
endif()

if(NOT TARGET einheit)
  message(FATAL_ERROR
    "einheit-cli sub-build did not expose an `einheit` target")
endif()
