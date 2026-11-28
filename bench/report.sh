#!/bin/sh
# Runs the comparisons and saves the output, for the report.
#   ./report.sh                    everything (about an hour and a half)
#   ./report.sh hugepages json     just compare_hugepages.sh and compare_json.sh
# Names: compare extra malloc more bumpalo suite protobuf hugepages json.
# compare_extra.sh needs llvm-18-dev and ~/programming/vmcontainer, compare_malloc.sh
# the malloc packages, compare_bumpalo.sh cargo (rustup), compare_protobuf.sh
# libprotobuf-dev, compare_json.sh rapidjson-dev; a script that fails is noted and
# skipped. compare_hugepages.sh reuses the builds of compare.sh, compare_more.sh and
# compare_suite.sh, and needs sudo for its second pass (asked for once, at the start).
ALL="compare extra malloc more bumpalo suite protobuf hugepages json"
WANT=${*:-$ALL}
for w in $WANT; do
    case " $ALL " in *" $w "*) ;; *) echo "unknown: $w (one of: $ALL)"; exit 1;; esac
done

# Ask for sudo now rather than an hour in, and keep it fresh.
keepalive=""
case " $WANT " in *" hugepages "*)
    if [ "${HUGE_ALWAYS:-1}" = 1 ] && sudo -v; then
        while sleep 60; do sudo -n -v 2> /dev/null || break; done &
        keepalive=$!
    fi;;
esac

mkdir -p results
out=results/$(date +%Y-%m-%d_%H%M).txt
{
    echo "topstack comparison, $(date)"
    echo "machine: $(uname -sr), $(nproc) cores"
    echo "compilers: $(g++ --version | head -1); $(clang++ --version | head -1)"
    echo "scripts: $WANT"
    for w in $WANT; do
        s=./compare_$w.sh
        [ $w = compare ] && s=./compare.sh
        echo
        $s || echo "($s failed)"
    done
} 2>&1 | tee "$out"
[ -n "$keepalive" ] && kill $keepalive 2> /dev/null
echo
echo "saved to bench/$out"
