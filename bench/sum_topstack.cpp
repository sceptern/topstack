// Goose's sum benchmark (bench/goose/sum.goose) with topstack: a control with no
// links and no variable sizes, so it only shows what the GrowBuf itself costs. The
// array grows in place on its own stack, so it needs no reserve() to end up at the
// exact size, and no element is ever copied to a bigger block.
#include "bench.h"
#include <topstack/grow_buf.hpp>

#include <cstdlib>

int main() {
    topstack::GrowBuf<int32_t> data;
    uint64_t r = 12345;
    for (long long i = 0; i < BENCH_N; i++) {
        r = xs_next(r);
        if (!data.push((int32_t)xs_mod(r, 1000))) std::abort();
    }
    long long total = 0;
    for (long long p = 0; p < 8; p++)
        for (int32_t x : data) total += x ^ p;
    emit((long long)data.size());
    emit(total);
    return 0;
}
