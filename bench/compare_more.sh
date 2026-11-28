#!/bin/sh
# Goose's tree, interp and sexp benchmarks: Goose, its C++ and Rust versions, and
# topstack: *_ts with Ref<T> links (checks off in release; *_refchk keeps them on),
# *_ts16 with Ref16<T> (2-byte) links, *_ts8 with Link8<T> (1-byte) links.
# Sizes are Goose's "large" ones. Run from the bench folder.
# C++ programs link the C++ standard library statically (-static-libstdc++), as
# Rust does with its own: loading libstdc++.so costs every C++ program about
# 2.4 MB of memory that has nothing to do with what is being measured.
set -e
GOOSE=${GOOSE:-$HOME/programming/goose/build/goose}
GB=${GB:-$HOME/programming/goose/bench}
TD=${TD:-24}; ID=${ID:-24}; SN=${SN:-300000}
B=bin/more
mkdir -p $B
F="-std=c++20 -O2 -DNDEBUG -static-libstdc++ -static-libgcc"

for f in tree interp sexp rng; do cp "$GB/goose/$f.goose" $B/; done
for f in tree interp sexp; do cp "$GB/cpp/$f.cpp" .; done
sed -i "s/^let DEPTH = [0-9]*;/let DEPTH = $TD;/" $B/tree.goose
sed -i "s/^let DEPTH = [0-9]*;/let DEPTH = $ID;/" $B/interp.goose
sed -i "s/^let N = [0-9]*;/let N = $SN;/" $B/sexp.goose
# Goose with both C compilers (*_goose is gcc, *_goose_clang clang), and with
# variable-width links (&<varint>, *_goose_varint*), the smallest Goose makes these.
for f in tree interp sexp; do
    sed 's/&<u32>/\&<varint>/g' $B/$f.goose > $B/${f}_varint.goose
    for cc in gcc clang; do
        s=; [ $cc = clang ] && s=_clang
        "$GOOSE" build --cc $cc $B/$f.goose -o $B/${f}_goose$s > /dev/null
        "$GOOSE" build --cc $cc $B/${f}_varint.goose -o $B/${f}_goose_varint$s > /dev/null
    done
done

for cxx in g++ clang++; do
    c=${cxx%++}; [ $c = g ] && c=gcc
    for v in 0 1 2; do $cxx $F -DBENCH_N=$TD -DVARIANT=$v tree.cpp -o $B/tree_cpp$v-$c; done
    for v in 0 1 2; do $cxx $F -DBENCH_N=$ID -DVARIANT=$v interp.cpp -o $B/interp_cpp$v-$c; done
    for v in 0 1; do $cxx $F -DBENCH_N=$SN -DVARIANT=$v sexp.cpp -o $B/sexp_cpp$v-$c; done
    $cxx $F -DBENCH_N=$TD -I ../include tree_topstack.cpp -o $B/tree_ts-$c
    $cxx $F -DBENCH_N=$ID -I ../include interp_topstack.cpp -o $B/interp_ts-$c
    $cxx $F -DBENCH_N=$SN -I ../include sexp_topstack.cpp -o $B/sexp_ts-$c
    $cxx $F -DBENCH_N=$TD -I ../include tree_ref16.cpp -o $B/tree_ts16-$c
    $cxx $F -DBENCH_N=$TD -I ../include tree_link8.cpp -o $B/tree_ts8-$c
    $cxx $F -DBENCH_N=$ID -I ../include interp_ref16.cpp -o $B/interp_ts16-$c
    $cxx $F -DBENCH_N=$ID -I ../include interp_link8.cpp -o $B/interp_ts8-$c
    $cxx $F -DBENCH_N=$SN -I ../include sexp_ref16.cpp -o $B/sexp_ts16-$c
done
g++ $F -DTOPSTACK_CHECKED_REFS -DBENCH_N=$TD -I ../include tree_topstack.cpp -o $B/tree_ts_refchk-gcc
g++ $F -DTOPSTACK_CHECKED_REFS -DBENCH_N=$ID -I ../include interp_topstack.cpp -o $B/interp_ts_refchk-gcc
g++ $F -DTOPSTACK_CHECKED_REFS -DBENCH_N=$SN -I ../include sexp_topstack.cpp -o $B/sexp_ts_refchk-gcc

if command -v rustc > /dev/null; then
    mkdir -p $B/rs
    cp "$GB/rust/bench.rs" $B/rs/
    for f in tree_box tree_arena interp_box interp_arena; do
        n=$TD; case $f in interp*) n=$ID;; esac
        sed "s/^const DEPTH: i64 = [0-9]*;/const DEPTH: i64 = $n;/" "$GB/rust/$f.rs" > $B/rs/$f.rs
        rustc -O -C codegen-units=1 $B/rs/$f.rs -o $B/${f%%_*}_rust_${f#*_} 2> /dev/null
    done
    for f in sexp_box sexp_arena; do
        sed "s/^const N: i64 = [0-9]*;/const N: i64 = $SN;/" "$GB/rust/$f.rs" > $B/rs/$f.rs
        rustc -O -C codegen-units=1 $B/rs/$f.rs -o $B/sexp_rust_${f#*_} 2> /dev/null
    done
fi

for k in tree interp sexp; do
    expect=$(./$B/${k}_goose | md5sum)
    for b in $B/${k}_*; do
        case $b in *.goose) continue;; esac
        [ "$(./$b | md5sum)" = "$expect" ] || { echo "WRONG OUTPUT: $b"; exit 1; }
    done
done
echo "all outputs match Goose"
for k in tree interp sexp; do
    case $k in tree) what="depth $TD";; interp) what="depth $ID";; sexp) what="N = $SN";; esac
    echo; echo "== $k, $what"
    python3 timeit.py $(ls $B/${k}_* | grep -v '\.goose$' | sed 's|^|./|')
done
