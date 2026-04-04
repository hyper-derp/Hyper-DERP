# System packages — no FetchContent needed when running on
# a properly provisioned build host.
#
# Required system packages:
#   libspdlog-dev libgtest-dev libgmock-dev libcli11-dev
#   libssl-dev libasio-dev

find_package(spdlog REQUIRED)
find_package(GTest REQUIRED)
find_package(CLI11 REQUIRED)

# ---- FetchContent dependencies ----------------------------------------------

include(FetchContent)

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
