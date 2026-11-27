// Goose's tree benchmark (bench/goose/tree.goose) with topstack. As in Goose, the
// nodes live in one growing array and link to each other with self-relative
// 32-bit offsets (Ref<T>), so a node is 12 bytes; children are pushed before
// their parent. The array is a global, like Goose's `pool`.
#include "bench.h"
#include <topstack/grow_buf.hpp>
#include <topstack/ref.hpp>

#ifndef DEPTH
#define DEPTH BENCH_N
#endif

struct TNode { int32_t v; topstack::Ref<TNode> l, r; };
static_assert(sizeof(TNode) == 12);

static topstack::GrowBuf<TNode> pool;

static TNode* build(int depth, uint64_t seed) {
    if (depth == 0) return pool.push((int32_t)xs_mod(seed, 1000), nullptr, nullptr);
    TNode* l = build(depth - 1, xs_next(seed));
    TNode* r = build(depth - 1, xs_next(seed + 1));
    return pool.push((int32_t)xs_mod(seed, 1000), l, r);
}
static long long sum_tree(const TNode* n, long long p) {
    long long s = n->v ^ p;
    if (n->l) s += sum_tree(n->l.get(), p);
    if (n->r) s += sum_tree(n->r.get(), p);
    return s;
}

int main() {
    TNode* root = build(DEPTH, 12345);
    long long total = 0;
    for (long long p = 0; p < 8; p++) total += sum_tree(root, p);
    emit((long long)pool.size());
    emit(total);
    return 0;
}
