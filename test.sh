#!/bin/sh
# Builds every test twice (normally, and with AddressSanitizer and
# UndefinedBehaviorSanitizer) and runs it. Stops with a non-zero exit status at
# the first failure, so CI notices.
set -e
mkdir -p build
F="-std=c++20 -Wall -Wextra -I include"
SAN="-O1 -g -fsanitize=address,undefined"

run() {
    # $1: program. Show only the last line, unless it failed: then show all of it.
    if ./"$1" > "$1.log" 2>&1; then
        tail -1 "$1.log"
    else
        cat "$1.log"
        echo "FAILED: $1"
        exit 1
    fi
}

for t in stack_test grow_buf_test stack_set_test ref_test ref16_test link8_test; do
    echo "== $t"
    g++ $F tests/$t.cpp -o build/$t
    run build/$t
    g++ $F $SAN tests/$t.cpp -o build/${t}_asan
    run build/${t}_asan
done
g++ $F tests/plain_only.cpp -o build/plain_only && ./build/plain_only && echo "== plain_only ok"
g++ $F tests/global_buf.cpp -o build/global_buf && ./build/global_buf && echo "== global_buf ok"
