#!/bin/sh
# Goose's push benchmark: Goose vs C++ vector/deque vs topstack GrowBuf.
# Run from the bench folder.
set -e
GOOSE=${GOOSE:-$HOME/programming/goose/build/goose}
N=${N:-64000000}
F="-std=c++20 -O2 -DNDEBUG -DBENCH_N=$N"
mkdir -p bin
sed "s/^let N = [0-9]*;/let N = $N;/" push.goose > bin/push_n.goose
cp rng.goose bin/
"$GOOSE" build bin/push_n.goose -o bin/push_goose > /dev/null
for cxx in g++ clang++; do
    $cxx $F -DVARIANT=0 push.cpp -o bin/push_vector_$cxx
    $cxx $F -DVARIANT=1 push.cpp -o bin/push_reserve_$cxx
    $cxx $F -DVARIANT=2 push.cpp -o bin/push_deque_$cxx
    $cxx $F -I ../include push_topstack.cpp -o bin/push_ts_$cxx
done

echo "outputs (all lines should be identical):"
for b in bin/push_goose bin/push_ts_* bin/push_vector_* bin/push_reserve_* bin/push_deque_*; do
    case $b in *.goose) continue;; esac
    echo "  $(./$b | md5sum | cut -c1-12)  $b"
done
python3 timeit.py ./bin/push_goose \
    ./bin/push_vector_g++ ./bin/push_reserve_g++ ./bin/push_deque_g++ ./bin/push_ts_g++ \
    ./bin/push_vector_clang++ ./bin/push_reserve_clang++ ./bin/push_deque_clang++ ./bin/push_ts_clang++
