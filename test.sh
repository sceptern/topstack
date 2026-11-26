#!/bin/sh
set -e
mkdir -p build
g++ -std=c++20 -Wall -Wextra -I include tests/stack_test.cpp -o build/stack_test
./build/stack_test | tail -1
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include tests/stack_test.cpp -o build/stack_asan
./build/stack_asan | tail -1
