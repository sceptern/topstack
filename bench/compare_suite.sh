#!/bin/sh
# The rest of Goose's benchmark suite (bench/run_bench.py in Goose): every Goose,
# C++ and Rust version Goose's own runner builds, plus topstack, at Goose's "large"
# sizes, Goose and C++ each built with gcc and with clang. Every output must match the
# first Goose version's, or the script stops.
# Run from the bench folder:
#   ./compare_suite.sh             all benchmarks
#   ./compare_suite.sh records     just one
# C++ links libstdc++ statically, as in compare_more.sh.
set -e
GOOSE=${GOOSE:-$HOME/programming/goose/build/goose}
GB=${GB:-$HOME/programming/goose/bench}
B=bin/suite
mkdir -p $B/rs
F="-std=c++20 -O2 -DNDEBUG -static-libstdc++ -static-libgcc"
RS="-O -C codegen-units=1"
ONLY=${1:-}

for f in rng vec std; do [ -f "$GB/goose/$f.goose" ] && cp "$GB/goose/$f.goose" $B/; done
cp "$GB/rust/bench.rs" $B/rs/

# name  size  Goose files  C++ variants  Rust files  topstack files
LIST="
sum       128000000 sum                      0,1    sum                                   sum_topstack
strlist   8000000   strlist                  0,1,2  strlist_owned,strlist_borrowed        strlist_topstack
records   16000000  records_var              0,1,2  records                               records_topstack
graph     2000000   graph,graph_csr          0,1,2  graph_nested,graph_csr,graph_arena    graph_topstack
words     16000000  words,words_dictionary   0,1,2  words_hashmap,words_open              words_topstack
particles 4000000   particles,particles_scalar 0,1,2 particles,particles_scalar          particles_topstack
lru       32000000  lru                      0,1    lru_hashmap,lru_open                  lru_topstack
scene     16        scene                    0,1    scene_box,scene_arena                 scene_topstack
calc      1600000   calc                     0,1    calc_box,calc_arena                   calc_topstack
respond   1600000   respond,respond_stream   0,1    respond_dto,respond_stream            respond_topstack
blur      8192      blur,blur_rows           0,1    blur_index,blur_windows               blur_topstack
"

echo "$LIST" | while read -r name n gfiles cvars rfiles tfiles; do
    [ -z "$name" ] && continue
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && continue
    rm -f $B/${name}_*
    for g in $(echo $gfiles | tr , ' '); do
        sed "s|^\(let [A-Z_]* = \)[0-9]*;\(.*BENCH_N.*\)|\1$n;\2|" "$GB/goose/$g.goose" > $B/$g.goose
        "$GOOSE" build --cc gcc $B/$g.goose -o $B/${name}_goose-$g > /dev/null
        "$GOOSE" build --cc clang $B/$g.goose -o $B/${name}_goose-$g-clang > /dev/null
    done
    cp "$GB/cpp/$name.cpp" $B/          # Goose's C++ version: build-time copy, not part of the repo
    for cxx in g++ clang++; do
        c=${cxx%++}; [ $c = g ] && c=gcc
        for v in $(echo $cvars | tr , ' '); do
            $cxx $F -DBENCH_N=$n -DVARIANT=$v -I . $B/$name.cpp -o $B/${name}_cpp$v-$c
        done
        for t in $(echo $tfiles | tr , ' '); do
            [ -f $t.cpp ] && $cxx $F -DBENCH_N=$n -I ../include $t.cpp -o $B/${name}_ts${t#*_topstack}-$c
        done
    done
    if command -v rustc > /dev/null; then
        for f in $(echo $rfiles | tr , ' '); do
            sed "s|^\(const [A-Z_]*: [a-z0-9]* = \)[0-9]*;\(.*BENCH_N.*\)|\1$n;\2|" \
                "$GB/rust/$f.rs" > $B/rs/$f.rs
            rustc $RS $B/rs/$f.rs -o $B/${name}_rust-$f 2> /dev/null
        done
    fi

    first=$(echo $gfiles | cut -d, -f1)
    expect=$(./$B/${name}_goose-$first | md5sum)
    for b in $B/${name}_*; do
        case $b in *.goose) continue;; esac
        [ "$(./$b | md5sum)" = "$expect" ] || { echo "WRONG OUTPUT: $b"; exit 1; }
    done
    echo; echo "== $name, size $n (all outputs match Goose)"
    python3 timeit.py $(ls $B/${name}_* | grep -v '\.goose$' | sed 's|^|./|')
done
