#!/bin/sh
# cJSON with topstack, three ways, against cJSON itself and RapidJSON, on the three
# files of the nativejson-benchmark (twitter, citm_catalog, canada):
#   cjson         cJSON 1.7.19 as released (malloc, free, cJSON_Delete)
#   cjson_hooks   level 1: the same cJSON.c, unchanged, given a topstack stack through
#                 cJSON_InitHooks; a Scope frees each document
#   cjson_ts      level 2: cJSON's parser and printer ported to a topstack layout
#                 (5-byte headers, OptRef links, keys and strings inline)
#   cjson_link8   level 3: level 2 with 1-byte Link8 links, built bottom-up (parse-only)
#   *_fast        levels 2 and 3 with faster number and scanning code (std::from_chars,
#                 std::to_chars, 8 bytes at a time), same output: the layout and the
#                 parsing code measured apart
#   rapidjson*    RapidJSON's DOM: Parse, ParseInsitu, and ParseInsitu with its faster,
#                 not always exact, number parsing
# Each round parses the file, walks the tree and prints it unformatted; the *_parse
# builds skip the printing. All cJSON versions must print the same text, byte for byte,
# and every version must agree on the walk's checksums, or the script stops.
# Memory is measured with timeit.py --exact-rss (these programs use less than Python).
# Needs RapidJSON's headers (sudo apt install rapidjson-dev, or RAPIDJSON=/path/to/include);
# without them, the RapidJSON rows are skipped. The script downloads cJSON and the three
# JSON files into bench/json/data/ (not part of the repo). Run from the bench folder.
# About 6 minutes.
set -e
J=json                  # the sources
B=bin/json
D=json/data             # downloads
mkdir -p $B $D
for f in twitter citm_catalog canada; do
    [ -s $D/$f.json ] || curl -sSfL -o $D/$f.json \
        https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/$f.json
done
# cJSON itself (MIT License), downloaded at its v1.7.19 release; not part of this repo
CJ=https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19
mkdir -p $D/cjson
for f in cJSON.c cJSON.h LICENSE; do
    [ -s $D/cjson/$f ] || curl -sSfL -o $D/cjson/$f $CJ/$f
done

RJ=""
if [ -n "$RAPIDJSON" ]; then RJ="-I $RAPIDJSON"
elif [ ! -f /usr/include/rapidjson/document.h ]; then RJ=none; echo "(no RapidJSON headers: skipping RapidJSON)"
fi
[ "$RJ" != none ] && RJ="$RJ -Wno-deprecated-declarations"     # RapidJSON 1.1.0 uses std::iterator

C="-std=c++20 -O2 -DNDEBUG -DTOPSTACK_NO_HUGE_PAGES -I ../include -I $J -I $D"     # compiling
F="$C -static-libstdc++ -static-libgcc"                                          # and linking
rm -f $B/*
gcc -O2 peakrss.c -o bin/peakrss
echo "CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
for cc in gcc clang; do
    cxx=g++; [ $cc = clang ] && cxx=clang++
    $cc -O2 -c $D/cjson/cJSON.c -o $B/cJSON-$cc.o
    for print in 1 0; do
        s=""; [ $print = 0 ] && s=_parse
        P="-DPRINT=$print"
        $cxx $F $P -DHOOKS=0 $J/cjson.cpp $B/cJSON-$cc.o -o $B/cjson$s-$cc
        $cxx $F $P -DHOOKS=1 $J/cjson.cpp $B/cJSON-$cc.o -o $B/cjson_hooks$s-$cc
        $cxx $F $P $J/topstack.cpp -o $B/cjson_ts$s-$cc
        $cxx $F $P $J/link8.cpp -o $B/cjson_link8$s-$cc
        $cxx $F $P -DFAST=1 $J/topstack.cpp -o $B/cjson_ts_fast$s-$cc
        $cxx $F $P -DFAST=1 $J/link8.cpp -o $B/cjson_link8_fast$s-$cc
        if [ "$RJ" != none ]; then
            $cxx $F $P $RJ $J/rapidjson.cpp -o $B/rapidjson$s-$cc
            $cxx $F $P $RJ -DINSITU=1 $J/rapidjson.cpp -o $B/rapidjson_insitu$s-$cc
            $cxx $F $P $RJ -DINSITU=1 -DFLAGS=0 $J/rapidjson.cpp -o $B/rapidjson_fast$s-$cc
        fi
    done
done

# file  rounds
LIST="
twitter      200
citm_catalog 100
canada       10
"
echo "$LIST" | while read -r name n; do
    [ -z "$name" ] && continue
    export JSON_FILE=$D/$name.json JSON_ROUNDS=$n
    for s in "" _parse; do
        expect=$(./$B/cjson$s-gcc)
        for b in $B/*$s-gcc $B/*$s-clang; do
            case $b in *_parse-*) [ -z "$s" ] && continue;; esac
            out=$(./$b)
            case $b in
                */rapidjson_fast*) keep=3;;      # its numbers may be 1 bit off
                */rapidjson*) keep=4;;           # prints numbers its own way
                *) keep=99;;
            esac
            [ "$(echo "$out" | head -$keep)" = "$(echo "$expect" | head -$keep)" ] ||
                { echo "WRONG OUTPUT: $b on $name"; exit 1; }
        done
        echo
        if [ -z "$s" ]; then echo "== $name, $n rounds of parse + walk + print (all outputs match)"
        else echo "== $name, $n rounds of parse + walk (all outputs match)"; fi
        progs=$(for b in $B/*$s-gcc $B/*$s-clang; do
            case $b in *_parse-*) [ -z "$s" ] && continue;; esac
            printf './%s ' "$b"; done)
        python3 timeit.py --exact-rss $progs
    done
done
