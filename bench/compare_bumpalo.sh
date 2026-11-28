#!/bin/sh
# Rust with bumpalo (the arena crate real Rust code uses) on all four tree
# benchmarks, next to Goose, topstack and Goose's own std-only Rust arena rows.
# Needs cargo (from rustup); the first run downloads bumpalo from crates.io.
# Run from the bench folder after compare.sh and compare_more.sh, whose
# programs it reuses (the sizes here must match theirs).
set -e
command -v cargo > /dev/null || { echo "cargo not found: install Rust with rustup first"; exit 1; }
D=${D:-19}; TD=${TD:-24}; ID=${ID:-24}; SN=${SN:-300000}
P=rust_bumpalo
B=bin/cmp; M=bin/more; R=bin/bump
mkdir -p $P/src/bin $R
cp $P/templates/bench.rs $P/src/bench.rs
cp $P/templates/main.rs $P/src/main.rs
for b in bintrees:$D tree:$TD interp:$ID sexp:$SN; do
    n=${b%%:*}; v=${b#*:}
    sed "s/@N@/$v/" $P/templates/${n}_bumpalo.rs > $P/src/bin/${n}_bumpalo.rs
done
(cd $P && cargo build --release --quiet)
for n in bintrees tree interp sexp; do cp $P/target/release/${n}_bumpalo $R/; done

check() { [ "$(./$1 | md5sum)" = "$(./$2 | md5sum)" ] || { echo "WRONG OUTPUT: $1"; exit 1; }; }
check $R/bintrees_bumpalo $B/bt_goose
for k in tree interp sexp; do check $R/${k}_bumpalo $M/${k}_goose; done
echo "all outputs match Goose"

# Only the programs that exist (Goose's Rust rows need rustc too).
some() { for f in "$@"; do [ -x "$f" ] && printf './%s ' "$f"; done; }
echo; echo "== bintrees, depth $D"
python3 timeit.py $(some $B/bt_goose $B/bt_goose_clang $B/bt_goose_varint $B/bt_goose_varint_clang $B/bt_ts_ref_gcc $B/bt_ts_ref_clang $B/bt_ts_ref16_gcc $B/bt_ts_ref16_clang $B/bt_ts_link8_gcc $B/bt_ts_link8_clang $B/bt_ts_ptr_gcc $B/bt_ts_ptr_clang $B/bt_rust_arena $R/bintrees_bumpalo)
for k in tree interp sexp; do
    case $k in tree) what="depth $TD";; interp) what="depth $ID";; sexp) what="N = $SN";; esac
    echo; echo "== $k, $what"
    python3 timeit.py $(some $M/${k}_goose $M/${k}_goose_clang $M/${k}_goose_varint $M/${k}_goose_varint_clang $M/${k}_ts-gcc $M/${k}_ts-clang $M/${k}_ts16-gcc $M/${k}_ts16-clang $M/${k}_ts8-gcc $M/${k}_ts8-clang $M/${k}_rust_arena $R/${k}_bumpalo)
done
