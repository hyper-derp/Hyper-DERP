# CMake toolchain file for ARM64 (aarch64) cross-compilation.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake ..
#
# Prerequisites:
#   sudo apt install gcc-14-aarch64-linux-gnu g++-14-aarch64-linux-gnu
#   sudo apt install liburing-dev:arm64 libsodium-dev:arm64
#
# Target platforms: AWS Graviton, Ampere (Oracle/Azure).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-14)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-14)
set(CMAKE_FIND_ROOT_PATH
  /usr/aarch64-linux-gnu
  /usr/lib/aarch64-linux-gnu
  /usr/include/aarch64-linux-gnu
  /usr
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
