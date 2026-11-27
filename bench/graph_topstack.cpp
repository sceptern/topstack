// Goose's graph benchmark (bench/goose/graph.goose) with topstack: one pool of edges
// that grows an edge at a time, each vertex's edges chained with 4-byte self-relative
// links (OptRef<Edge>). The first V entries of the pool are the list heads, as in Goose:
// a OptRef can only reach within its own stack, so the heads live in the pool too. Growth
// never moves the pool, so no link ever needs fixing, and no reserve() is needed.
#include "bench.h"
#include <topstack/grow_buf.hpp>
#include <topstack/ref.hpp>

#include <cstdlib>

using topstack::GrowBuf;
using topstack::OptRef;

static const long long DEG = 8;
static const int SOURCES = 4;

struct Edge { uint32_t to; OptRef<Edge> next; };

int main() {
    const long long nv = BENCH_N, ne = nv * DEG;
    GrowBuf<Edge> pool;
    for (long long v = 0; v < nv; v++)
        if (!pool.push(0u, nullptr)) std::abort();              // list heads
    uint64_t r = 12345;
    for (long long e = 0; e < ne; e++) {
        r = xs_next(r);
        Edge& head = pool[(size_t)xs_mod(r, (uint64_t)nv)];
        Edge* ne_ = pool.push((uint32_t)xs_mod(r >> 24, (uint64_t)nv), head.next.get());
        if (!ne_) std::abort();
        head.next = ne_;
    }

    GrowBuf<int32_t> dist;
    for (long long v = 0; v < nv; v++)
        if (!dist.push(-1)) std::abort();
    GrowBuf<uint32_t> q;
    long long total = 0;
    uint64_t seed = 999;
    for (int s = 0; s < SOURCES; s++) {
        seed = xs_next(seed);
        uint32_t src = (uint32_t)xs_mod(seed, (uint64_t)nv);
        for (int32_t& d : dist) d = -1;
        q.clear();
        dist[src] = 0;
        if (!q.push(src)) std::abort();
        for (size_t qh = 0; qh < q.size(); qh++) {
            uint32_t u = q[qh];
            int32_t d = dist[u];
            total += d;
            for (const Edge* c = pool[u].next.get(); c; c = c->next.get()) {
                uint32_t w = c->to;
                if (dist[w] < 0) {
                    dist[w] = d + 1;
                    if (!q.push(w)) std::abort();
                }
            }
        }
    }
    emit(nv + ne);
    emit(total);
    return 0;
}
