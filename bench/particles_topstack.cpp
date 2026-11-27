// Goose's particles benchmark (bench/goose/particles.goose) with topstack: an array
// of structs updated in place, mostly arithmetic. The array grows in place on its own
// stack (no reserve() needed); the update loop is the same per-component one as the
// expert C++ version.
#include "bench.h"
#include <topstack/grow_buf.hpp>

#include <cstdlib>

static const int STEPS = 200;

struct F3 { float x, y, z; };
struct P { F3 pos, vel; };

int main() {
    const long long n = BENCH_N;
    topstack::GrowBuf<P> ps;
    uint64_t r = 12345;
    for (long long i = 0; i < n; i++) {
        r = xs_next(r);
        P p;
        p.pos.x = (float)xs_mod(r, 1024);
        p.pos.y = (float)xs_mod(r >> 12, 1024);
        p.pos.z = (float)xs_mod(r >> 24, 1024);
        p.vel.x = (float)((double)((long long)xs_mod(r >> 36, 256) - 128) / 16.0);
        p.vel.y = (float)((double)((long long)xs_mod(r >> 44, 256) - 128) / 16.0);
        p.vel.z = (float)((double)((long long)xs_mod(r >> 52, 256) - 128) / 16.0);
        if (!ps.push(p)) std::abort();
    }
    for (int s = 0; s < STEPS; s++) {
        for (P& p : ps) {
            p.vel.y += -0.0625f;
            p.pos.x += p.vel.x;
            p.pos.y += p.vel.y;
            p.pos.z += p.vel.z;
            if (p.pos.y < 0.0f) { p.pos.y = -p.pos.y; p.vel.y = -(p.vel.y * 0.5f); }
        }
    }
    long long total = 0;
    for (const P& p : ps)
        total += (long long)(p.pos.x * 16.0f) + (long long)(p.pos.y * 16.0f) + (long long)(p.pos.z * 16.0f);
    emit(n);
    emit(total);
    return 0;
}
