#!/bin/sh
# What huge pages add, on the benchmarks with big data. The other scripts build
# topstack with huge pages off, like everything else they measure; this one shows each
# side with and without them:
#   *_ts            topstack as shipped: asks for huge pages beyond each stack's first 2 MB
#   *_ts_nohuge     topstack built with TOPSTACK_NO_HUGE_PAGES
#   *_huge          the C++ version with glibc's huge pages on
#                   (GLIBC_TUNABLES=glibc.malloc.hugetlb=1); without it, glibc doesn't ask
#   *_goose*        Goose, whose runtime turns huge pages off for each stack's first 2 MB
#                   and never asks for the rest
# It runs everything twice, with the system's huge-page setting at:
#   madvise   only programs that ask get huge pages (Ubuntu's default)
#   always    every program gets them, Goose and plain C++ included: like for like
# The second pass needs sudo to change the setting; the original setting is put back
# at the end, even if the run fails. HUGE_ALWAYS=0 skips the second pass.
# Run from the bench folder, after compare.sh, compare_more.sh and compare_suite.sh,
# whose Goose and C++ builds it reuses. About 20 minutes.
set -e
B=bin/huge
M=bin/more
S=bin/suite
THP=${THP_DIR:-/sys/kernel/mm/transparent_hugepage}   # THP_DIR: for testing the script
mkdir -p $B
F="-std=c++20 -O2 -DNDEBUG -static-libstdc++ -static-libgcc -I ../include"

# name  size  topstack source  Goose reference  C++ builds to rerun with huge pages
LIST="
tree    24       tree_ref16.cpp     $M/tree_goose               $M/tree_cpp2-gcc,$M/tree_cpp2-clang
sexp    300000   sexp_topstack.cpp  $M/sexp_goose               $M/sexp_cpp1-gcc,$M/sexp_cpp1-clang
records 16000000 records_topstack.cpp $S/records_goose-records_var $S/records_cpp2-gcc,$S/records_cpp2-clang
graph   2000000  graph_topstack.cpp $S/graph_goose-graph        $S/graph_cpp2-gcc,$S/graph_cpp2-clang
words   16000000 words_topstack.cpp $S/words_goose-words        $S/words_cpp2-gcc,$S/words_cpp2-clang
lru     32000000 lru_topstack.cpp   $S/lru_goose-lru            $S/lru_cpp1-gcc,$S/lru_cpp1-clang
scene   16       scene_topstack.cpp $S/scene_goose-scene        $S/scene_cpp1-gcc,$S/scene_cpp1-clang
"

# The programs to time for one benchmark (Goose, C++, C++ with huge pages, topstack).
progs_of() {
    name=$1 goose=$2 cpps=$3
    progs="$goose ${goose}_clang"
    [ -x "${goose}-clang" ] && progs="$goose ${goose}-clang"
    for cpp in $(echo $cpps | tr , ' '); do
        progs="$progs $cpp $B/$(basename $cpp)_huge"
    done
    for p in $progs $B/${name}_ts*; do [ -x "$p" ] && printf './%s ' "$p"; done
}

# Build, and check every output against Goose's.
echo "$LIST" | while read -r name n src goose cpps; do
    [ -z "$name" ] && continue
    rm -f $B/${name}_*
    for cxx in g++ clang++; do
        c=${cxx%++}; [ $c = g ] && c=gcc
        $cxx $F -DBENCH_N=$n $src -o $B/${name}_ts-$c
        $cxx $F -DTOPSTACK_NO_HUGE_PAGES -DBENCH_N=$n $src -o $B/${name}_ts_nohuge-$c
    done
    for cpp in $(echo $cpps | tr , ' '); do
        w=$B/$(basename $cpp)_huge
        printf '#!/bin/sh\nGLIBC_TUNABLES=glibc.malloc.hugetlb=1 exec %s "$@"\n' "$PWD/$cpp" > $w
        chmod +x $w
    done
    expect=$(./$goose | md5sum)
    for b in $(progs_of $name $goose $cpps); do
        [ "$($b | md5sum)" = "$expect" ] || { echo "WRONG OUTPUT: $b"; exit 1; }
    done
done

setting() { sed 's/.*\[\(.*\)\].*/\1/' $THP/enabled 2>/dev/null || echo unknown; }

run_pass() {
    echo
    echo "#### system huge-page setting: $(setting) (defrag: $(sed 's/.*\[\(.*\)\].*/\1/' $THP/defrag 2>/dev/null || echo unknown))"
    echo "$LIST" | while read -r name n src goose cpps; do
        [ -z "$name" ] && continue
        echo; echo "== $name (all outputs match Goose)"
        python3 timeit.py $(progs_of $name $goose $cpps)
    done
}

original=$(setting)
keepalive=""
put_back() {
    [ -n "$keepalive" ] && kill $keepalive 2> /dev/null
    [ "$(setting)" = "$original" ] || echo "$original" | sudo tee $THP/enabled > /dev/null
}
trap put_back EXIT
trap 'exit 1' INT TERM
# Ask for the sudo password now, not after the first pass, and keep it fresh.
if [ "${HUGE_ALWAYS:-1}" = 1 ] && sudo -v; then
    while sleep 60; do sudo -n -v 2> /dev/null || break; done &
    keepalive=$!
fi

if [ "$original" != madvise ] && [ "$original" != unknown ]; then
    echo madvise | sudo tee $THP/enabled > /dev/null || { echo "(can't set madvise: skipping)"; exit 1; }
fi
run_pass
if [ "${HUGE_ALWAYS:-1}" = 1 ]; then
    if echo always | sudo tee $THP/enabled > /dev/null; then
        run_pass
    else
        echo; echo "(can't change the huge-page setting without sudo: skipping the 'always' pass)"
    fi
fi
