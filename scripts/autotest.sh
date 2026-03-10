#!/usr/bin/env bash
# Rebuild and run tests on file change.
build_dir="build"

verbose=0
stoponfail=0
outputonfail=0

while getopts "hvso" flag; do
  case $flag in
    h|help)
      echo "$0 - Build and test on file change."
      echo "  -h - Display this help."
      echo "  -v - Verbose test output."
      echo "  -s - Stop on first failure."
      echo "  -o - Output on failure."
      exit 0
    ;;
    v) verbose=1 ;;
    s) stoponfail=1 ;;
    o) outputonfail=1 ;;
    \?) echo "Invalid option." ;;
  esac
done

ctest_opts=""
[ $verbose -eq 1 ] && ctest_opts="$ctest_opts -V"
[ $stoponfail -eq 1 ] && ctest_opts="$ctest_opts --stop-on-failure"
[ $outputonfail -eq 1 ] && ctest_opts="$ctest_opts --output-on-failure"

cmd="cmake --build $build_dir -j"
cmd="$cmd && ctest $ctest_opts -j --test-dir $build_dir/tests"

while :
do
  find . -name "*.cc" -o -name "*.h" -o -name "*.txt" \
    -o -name "*.cmake" \
    -not -path "./build*" -not -path "./.git/*" \
    | entr -s "$cmd"
  sleep 2
done
