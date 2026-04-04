# Contributing to Hyper-DERP

Thank you for your interest in contributing. This document covers
the process for building, testing, and submitting changes.

## Building

Hyper-DERP requires Linux with kernel 6.1+ (for io_uring
DEFER_TASKRUN + SINGLE_ISSUER), a C++23 compiler (Clang 19
or GCC 14), and the following dependencies:

```bash
sudo apt install cmake ninja-build \
  liburing-dev libsodium-dev libspdlog-dev \
  libgtest-dev libgmock-dev libcli11-dev \
  libssl-dev libasio-dev
```

Build with CMake:

```bash
cmake --preset default
cmake --build build -j
```

For a debug build:

```bash
cmake --preset debug
cmake --build build-debug -j
```

For ARM64 cross-compilation:

```bash
sudo apt install gcc-14-aarch64-linux-gnu \
  g++-14-aarch64-linux-gnu \
  liburing-dev:arm64 libsodium-dev:arm64
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
  -DBUILD_TESTING=OFF -B build-arm64
cmake --build build-arm64 -j
```

## Running Tests

```bash
ctest --test-dir build-debug --output-on-failure
```

## Code Style

All C++ code follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
with these specifics:

- **Line limit:** 80 characters.
- **Indentation:** 2 spaces (no tabs).
- **Docstrings:** required on all public classes, functions,
  and methods in headers.
- **Comments:** place on the line above the code, not inline.

Python scripts follow the
[Google Python Style Guide](https://google.github.io/styleguide/pyguide.html).

## Linting

Run all linters before submitting a PR:

```bash
# C++ lint
cpplint --recursive src/ include/ tools/ tests/
```

Fix all warnings. CI runs test, lint, and package
automatically on every push and PR.

## Submitting Changes

1. **Fork** the repository and create a feature branch:
   ```bash
   git checkout -b my-feature
   ```

2. **Make your changes.** Keep commits focused and atomic.

3. **Test** your changes (see above).

4. **Lint** your changes (see above).

5. **Push** your branch and open a pull request against `master`.

6. **Describe** your PR clearly: what it does, why, and how
   to verify it. Link any related issues.

7. A maintainer will review your PR. Address feedback with
   additional commits (do not force-push during review).

## Reporting Bugs

Open a GitHub issue with:

- Hyper-DERP version and commit hash.
- Linux kernel version (`uname -r`).
- Steps to reproduce.
- Expected vs. actual behavior.
- Relevant logs or error messages.

## Requesting Features

Open a GitHub issue describing:

- The problem you are trying to solve.
- Your proposed solution (if any).
- Alternatives you considered.

## License

Hyper-DERP is licensed under the MIT License. By submitting a
pull request you agree that your contribution is licensed under
the same terms. No CLA is required.
