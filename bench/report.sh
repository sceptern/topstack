#!/bin/sh
# Runs every comparison and saves the output, for the report.
# compare_extra.sh needs llvm-18-dev and ~/programming/vmcontainer, and
# compare_malloc.sh the malloc packages; each is skipped if its script fails.
# compare_more.sh (tree, interp, sexp) takes the longest: about 15 minutes.
# compare_bumpalo.sh needs cargo (rustup).
mkdir -p results
out=results/$(date +%Y-%m-%d_%H%M).txt
{
    echo "topstack comparison, $(date)"
    echo "machine: $(uname -sr), $(nproc) cores"
    echo "compilers: $(g++ --version | head -1); $(clang++ --version | head -1)"
    echo
    ./compare.sh
    echo
    ./compare_extra.sh || echo "(compare_extra.sh failed)"
    echo
    ./compare_malloc.sh || echo "(compare_malloc.sh failed)"
    echo
    ./compare_more.sh || echo "(compare_more.sh failed)"
    echo
    ./compare_bumpalo.sh || echo "(compare_bumpalo.sh failed)"
} 2>&1 | tee "$out"
echo
echo "saved to bench/$out"
