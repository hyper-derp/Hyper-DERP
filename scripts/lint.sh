#!/usr/bin/env bash

filter='--filter=-whitespace/ending_newline,-build/include_order,-build/c++11'
echo '--- CPP Lint SRC ---'
cpplint --root=src $filter --recursive src/*
echo '--- CPP Lint TESTS ---'
cpplint --root=src $filter --recursive tests/*
echo '--- CPP Lint INCLUDE ---'
cpplint --root=include $filter --recursive include/*
