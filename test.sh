#!/bin/sh
set -e
mkdir -p build
F="-std=c++20 -Wall -Wextra -I include"
SAN="-O1 -g -fsanitize=address,undefined"

for t in stack_test grow_buf_test stack_set_test ref_test; do
    echo "== $t"
    g++ $F tests/$t.cpp -o build/$t
    ./build/$t | tail -1
    g++ $F $SAN tests/$t.cpp -o build/${t}_asan
    ./build/${t}_asan | tail -1
done
g++ $F tests/plain_only.cpp -o build/plain_only && ./build/plain_only && echo "== plain_only ok"
g++ $F tests/global_buf.cpp -o build/global_buf && ./build/global_buf && echo "== global_buf ok"
