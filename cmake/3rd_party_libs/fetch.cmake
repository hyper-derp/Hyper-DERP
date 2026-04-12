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
