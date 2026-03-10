#!/usr/bin/env bash

find include tests src \( -name '*.h' -o -name '*.cc' \) -exec clang-format -i {} +
