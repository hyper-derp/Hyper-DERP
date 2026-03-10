# System packages — no FetchContent needed when running on
# a properly provisioned build host.
#
# Required system packages:
#   libspdlog-dev libgtest-dev libgmock-dev libcli11-dev

find_package(spdlog REQUIRED)
find_package(GTest REQUIRED)
find_package(CLI11 REQUIRED)
