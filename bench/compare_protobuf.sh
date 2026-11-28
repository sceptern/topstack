#!/bin/sh
# bintrees with Google's protobuf Arena, next to topstack and the other arenas.
# Needs, once:  sudo apt install libprotobuf-dev
# Run from the bench folder after compare.sh (it reuses its programs).
# C++ programs link libstdc++ statically, as in compare.sh. If the static
# libprotobuf-lite.a is installed it is used, so the comparison stays fair;
# otherwise the shared library is used, which adds about 2.4 MB of memory
# (loading libstdc++.so) to the protobuf rows.
set -e
B=bin/cmp
D=${D:-19}
LIBDIR=${LIBDIR:-/usr/lib/x86_64-linux-gnu}
if [ -f "$LIBDIR/libprotobuf-lite.a" ]; then
    F="-std=c++20 -O2 -DNDEBUG -static-libstdc++ -static-libgcc"
    L="$LIBDIR/libprotobuf-lite.a -lpthread"
else
    F="-std=c++20 -O2 -DNDEBUG"
    L="-lprotobuf-lite -lpthread"
    echo "(libprotobuf-lite.a not found: using the shared library)"
fi
for cxx in g++ clang++; do
    c=${cxx%++}; [ $c = g ] && c=gcc
    for v in 0 1 2; do
        $cxx $F -DBENCH_N=$D -DVARIANT=$v bintrees_protobuf.cpp $L -o $B/bt_protobuf${v}_$c
    done
done
expect=$(./$B/bt_goose | md5sum)
for b in $B/bt_protobuf*; do
    [ "$(./$b | md5sum)" = "$expect" ] || { echo "WRONG OUTPUT: $b"; exit 1; }
done
echo "all outputs match Goose"
some() { for f in "$@"; do [ -x "$f" ] && printf './%s ' "$f"; done; }
echo; echo "== bintrees, depth $D: protobuf Arena (0 per tree, 1 Reset, 2 reused buffer)"
python3 timeit.py $(some $B/bt_goose $B/bt_goose_clang $B/bt_ts_ref_gcc $B/bt_ts_ref_clang $B/bt_ts_ptr_gcc $B/bt_ts_ptr_clang \
    $B/bt_pmr_gcc $B/bt_pmr_reuse_gcc $B/bt_llvm_gcc $B/bt_protobuf0_gcc $B/bt_protobuf1_gcc \
    $B/bt_protobuf2_gcc $B/bt_protobuf0_clang $B/bt_protobuf1_clang $B/bt_protobuf2_clang)
