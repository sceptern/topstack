// Goose's blur benchmark (bench/goose/blur.goose) with topstack: two images built by
// pushing pixels, then a 3x3 blur back and forth. Both images grow in place, each on
// its own stack, while they are built side by side, as in the Goose version. The blur
// itself is the row-pointer loop of the expert C++ version.
#include "bench.h"
#include <topstack/grow_buf.hpp>

#include <cstdlib>

static const long long W = BENCH_N;
static const int PASSES = 16;

static void blur(const uint8_t* __restrict src, uint8_t* __restrict dst) {
    for (long long y = 1; y < W - 1; y++) {
        const uint8_t* __restrict r0 = src + (y - 1) * W;
        const uint8_t* __restrict r1 = r0 + W;
        const uint8_t* __restrict r2 = r1 + W;
        uint8_t* __restrict d = dst + y * W;
        for (long long x = 1; x < W - 1; x++) {
            int s = r0[x - 1]     + r0[x] * 2 + r0[x + 1]
                  + r1[x - 1] * 2 + r1[x] * 4 + r1[x + 1] * 2
                  + r2[x - 1]     + r2[x] * 2 + r2[x + 1];
            d[x] = (uint8_t)(s >> 4);
        }
    }
}

int main() {
    topstack::GrowBuf<uint8_t> a, b;
    uint64_t r = 12345;
    for (long long i = 0; i < W * W; i++) {
        r = xs_next(r);
        if (!a.push((uint8_t)xs_mod(r, 256)) || !b.push(0)) std::abort();
    }
    for (int p = 0; p < PASSES / 2; p++) { blur(a.data(), b.data()); blur(b.data(), a.data()); }
    long long total = 0;
    for (uint8_t v : a) total += v;
    emit(W);
    emit(total);
    return 0;
}
