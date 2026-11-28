#!/bin/sh
# topstack against Goose, Goose's expert C++ and Rust versions, and std::pmr.
# bt_ts_ref: Ref<T> links (checks off in release); bt_ts_refchk: TOPSTACK_CHECKED_REFS;
# bt_ts_ref16: Ref16<T> (2-byte) links. bt_ts_link8: Link8<T> (1-byte) links.
# Run from the bench folder. Rust rows are skipped if rustc isn't installed.
# C++ programs link the C++ standard library statically (-static-libstdc++), as
# Rust does with its own: loading libstdc++.so costs every C++ program about
# 2.4 MB of memory that has nothing to do with what is being measured.
set -e
GOOSE=${GOOSE:-$HOME/programming/goose/build/goose}
GB=${GB:-$HOME/programming/goose/bench}
D=19; N=64000000
mkdir -p bin/cmp
B=bin/cmp
# topstack is built with huge pages off (TOPSTACK_NO_HUGE_PAGES), like every other
# program here, so the tables compare like with like; compare_hugepages.sh measures
# what huge pages add.
F="-std=c++20 -O2 -DNDEBUG -static-libstdc++ -static-libgcc -DTOPSTACK_NO_HUGE_PAGES"

sed "s/^let MAXDEPTH = [0-9]*;/let MAXDEPTH = $D;/" bintrees.goose > $B/bintrees.goose
sed "s/^let N = [0-9]*;/let N = $N;/" push.goose > $B/push.goose
cp rng.goose $B/
# Goose with both C compilers, like the C++ rows. *_goose is gcc, *_goose_clang clang.
# bt_goose_varint*: Goose's bintrees with variable-width links (&<varint>), the
# smallest Goose can make it, to compare with topstack's 2- and 1-byte links.
sed 's/&<u32>/\&<varint>/g' $B/bintrees.goose > $B/bintrees_varint.goose
for cc in gcc clang; do
    s=; [ $cc = clang ] && s=_clang
    "$GOOSE" build --cc $cc $B/bintrees.goose -o $B/bt_goose$s > /dev/null
    "$GOOSE" build --cc $cc $B/bintrees_varint.goose -o $B/bt_goose_varint$s > /dev/null
    "$GOOSE" build --cc $cc $B/push.goose -o $B/push_goose$s > /dev/null
done
for cxx in g++ clang++; do
    c=${cxx%++}; [ $c = g ] && c=gcc
    $cxx $F -DBENCH_N=$D -DVARIANT=0 bintrees.cpp -o $B/bt_cpp_newdelete_$c
    $cxx $F -DBENCH_N=$D -DVARIANT=1 bintrees.cpp -o $B/bt_cpp_vector_$c
    $cxx $F -DBENCH_N=$D -DVARIANT=0 bintrees_pmr.cpp -o $B/bt_pmr_$c
    $cxx $F -DBENCH_N=$D -DVARIANT=1 bintrees_pmr.cpp -o $B/bt_pmr_reuse_$c
    $cxx $F -DBENCH_N=$D -DVARIANT=2 -I ../include bintrees_topstack.cpp -o $B/bt_ts_ptr_$c
    $cxx $F -DBENCH_N=$D -DVARIANT=3 -I ../include bintrees_topstack.cpp -o $B/bt_ts_u32_$c
    $cxx $F -DBENCH_N=$D -I ../include bintrees_ref.cpp -o $B/bt_ts_ref_$c
    $cxx $F -DBENCH_N=$D -DTOPSTACK_CHECKED_REFS -I ../include bintrees_ref.cpp -o $B/bt_ts_refchk_$c
    $cxx $F -DBENCH_N=$D -I ../include bintrees_ref16.cpp -o $B/bt_ts_ref16_$c
    $cxx $F -DBENCH_N=$D -I ../include bintrees_link8.cpp -o $B/bt_ts_link8_$c
    $cxx $F -DBENCH_N=$D bintrees_thread.cpp -o $B/bt_threaded_$c
    $cxx $F -DBENCH_N=$N -DVARIANT=1 push.cpp -o $B/push_cpp_reserve_$c
    $cxx $F -DBENCH_N=$N -I ../include push_topstack.cpp -o $B/push_ts_$c
done
if command -v rustc > /dev/null; then
    mkdir -p $B/rs
    cp "$GB/rust/bench.rs" $B/rs/
    for f in bintrees_box bintrees_arena; do
        sed "s/^const MAXDEPTH: i64 = [0-9]*;/const MAXDEPTH: i64 = $D;/" "$GB/rust/$f.rs" > $B/rs/$f.rs
        rustc -O -C codegen-units=1 $B/rs/$f.rs -o $B/bt_rust_${f#bintrees_} 2> /dev/null
    done
    sed "s/^const N: i64 = [0-9]*;/const N: i64 = $N;/" "$GB/rust/push.rs" > $B/rs/push.rs
    rustc -O -C codegen-units=1 $B/rs/push.rs -o $B/push_rust_reserve 2> /dev/null
fi

for kind in bt push; do
    expect=$(./$B/${kind}_goose | md5sum)
    for b in $B/${kind}_*; do
        [ "$(./$b | md5sum)" = "$expect" ] || { echo "WRONG OUTPUT: $b"; exit 1; }
    done
done
echo "all outputs match Goose"
echo; echo "== bintrees, depth $D"
python3 timeit.py $(ls $B/bt_* | grep -v '\.goose$' | sed 's|^|./|')
echo; echo "== push, $N items"
python3 timeit.py $(ls $B/push_* | grep -v '\.goose$' | sed 's|^|./|')
