#!/bin/sh
# The per-node allocation versions of bintrees (C++ new/delete, Rust Box, and the
# allocators that get their memory from malloc) with faster mallocs swapped in
# through LD_PRELOAD, next to topstack and Goose. Nothing is rebuilt.
# Needs, once:  sudo apt install libmimalloc2.0 libjemalloc2 libtcmalloc-minimal4t64
# Run from the bench folder after compare.sh and compare_extra.sh.
set -e
B=bin/cmp
W=$B/malloc
mkdir -p $W
LIBDIR=${LIBDIR:-/usr/lib/x86_64-linux-gnu}
find_lib() { ls $LIBDIR/$1 2>/dev/null | head -1; }
MI=${MI:-$(find_lib 'libmimalloc.so*')}
JE=${JE:-$(find_lib 'libjemalloc.so*')}
TC=${TC:-$(find_lib 'libtcmalloc_minimal.so*')}

# One small script per (program, malloc). `exec` replaces the shell with the
# program, so the time and memory measured are the program's own.
rm -f $W/*
for prog in bt_cpp_newdelete_gcc bt_cpp_newdelete_clang bt_rust_box bt_pmr_gcc bt_llvm_gcc; do
    [ -x $B/$prog ] || continue
    for m in glibc mimalloc jemalloc tcmalloc; do
        case $m in
            glibc)    lib= ;;
            mimalloc) lib=$MI ;;
            jemalloc) lib=$JE ;;
            tcmalloc) lib=$TC ;;
        esac
        [ $m = glibc ] || [ -n "$lib" ] || continue
        f=$W/${prog#bt_}+$m
        if [ -n "$lib" ]; then
            printf '#!/bin/sh\nLD_PRELOAD=%s exec %s/%s\n' "$lib" "$PWD/$B" "$prog" > $f
        else
            printf '#!/bin/sh\nexec %s/%s\n' "$PWD/$B" "$prog" > $f
        fi
        chmod +x $f
    done
done
echo "mallocs found: mimalloc=${MI:-none} jemalloc=${JE:-none} tcmalloc=${TC:-none}"

expect=$(./$B/bt_goose | md5sum)
for f in $W/*; do
    [ "$(./$f | md5sum)" = "$expect" ] || { echo "WRONG OUTPUT: $f"; exit 1; }
done
echo "all outputs match Goose"
echo; echo "== bintrees, depth 19"
python3 timeit.py ./$B/bt_goose ./$B/bt_goose_clang ./$B/bt_ts_u32_gcc ./$B/bt_ts_ptr_gcc ./$B/bt_cpp_vector_gcc $(ls $W/* | sed 's|^|./|')
