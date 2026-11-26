#!/bin/sh
set -e
GOOSE=${GOOSE:-$HOME/programming/goose/build/goose}
D=19
F="-std=c++20 -O2 -DNDEBUG -DBENCH_N=$D"
mkdir -p bin
"$GOOSE" build bintrees.goose -o bin/goose
g++ $F -DVARIANT=0 bintrees.cpp -o bin/cpp_newdelete
g++ $F -DVARIANT=1 bintrees.cpp -o bin/cpp_vector
g++ $F -DVARIANT=2 -I ../include bintrees_topstack.cpp -o bin/ts_ptr
g++ $F -DVARIANT=3 -I ../include bintrees_topstack.cpp -o bin/ts_u32

echo "outputs (all lines should be identical):"
for b in goose cpp_newdelete cpp_vector ts_ptr ts_u32; do
    echo "  $(./bin/$b | md5sum | cut -c1-12)  $b"
done
python3 timeit.py ./bin/goose ./bin/cpp_newdelete ./bin/cpp_vector ./bin/ts_ptr ./bin/ts_u32
