#!/bin/sh
# topstack against LLVM's BumpPtrAllocator (bintrees) and pinned_vector (push).
# Needs, once:
#   sudo apt install llvm-18-dev
#   git clone https://github.com/mknejp/vmcontainer.git ~/programming/vmcontainer
# Run from the bench folder, after compare.sh (it reuses bin/cmp/bt_goose etc.).
set -e
VMC=${VMC:-$HOME/programming/vmcontainer}
B=bin/cmp
F="-std=c++20 -O2 -DNDEBUG"
LLVM="-I$(llvm-config-18 --includedir) -L$(llvm-config-18 --libdir) -lLLVM-18"
VM="-I $VMC/lib/include $VMC/lib/src/vmcontainer/vm.cpp"
mkdir -p $B
for cxx in g++ clang++; do
    c=${cxx%++}; [ $c = g ] && c=gcc
    $cxx $F -DBENCH_N=19 -DVARIANT=0 bintrees_llvm.cpp $LLVM -o $B/bt_llvm_$c
    $cxx $F -DBENCH_N=19 -DVARIANT=1 bintrees_llvm.cpp $LLVM -o $B/bt_llvm_reset_$c
    $cxx $F -DBENCH_N=64000000 push_pinned.cpp $VM -o $B/push_pinned_$c
done
for kind in bt push; do
    expect=$(./$B/${kind}_goose | md5sum)
    for b in $B/${kind}_llvm* $B/${kind}_pinned*; do
        [ -e "$b" ] || continue
        [ "$(./$b | md5sum)" = "$expect" ] || { echo "WRONG OUTPUT: $b"; exit 1; }
    done
done
echo "all outputs match Goose"
echo; echo "== bintrees, pointer nodes"
python3 timeit.py ./$B/bt_goose ./$B/bt_ts_ptr_gcc ./$B/bt_ts_ptr_clang ./$B/bt_ts_u32_gcc \
    ./$B/bt_llvm_gcc ./$B/bt_llvm_clang ./$B/bt_llvm_reset_gcc ./$B/bt_llvm_reset_clang
echo; echo "== push"
python3 timeit.py ./$B/push_goose ./$B/push_ts_gcc ./$B/push_ts_clang ./$B/push_pinned_gcc ./$B/push_pinned_clang
