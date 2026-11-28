// Goose's tree benchmark (bench/goose/tree.goose) with OptLink8<T>: 1-byte links. A
// child within 64 bytes is linked directly; a farther one through a 4-byte anchor made
// right after its parent. Children are made before their parent, so nearly every link
// is direct: a node is 6 bytes (8 with OptRef16<T>, 12 with OptRef<T>).
#include "bench.h"
#include <topstack/link8.hpp>

#ifndef DEPTH
#define DEPTH BENCH_N
#endif

struct TNode { topstack::Packed<int32_t> v; topstack::OptLink8<TNode> l, r; };
static_assert(sizeof(TNode) == 6);

static long long count = 0;

static TNode* build(topstack::Stack& s, int depth, uint64_t seed) {
    TNode* l = depth ? build(s, depth - 1, xs_next(seed)) : nullptr;
    TNode* r = depth ? build(s, depth - 1, xs_next(seed + 1)) : nullptr;
    count++;
    return s.make<TNode>((int32_t)xs_mod(seed, 1000), l, r);
}
static long long sum_tree(const TNode* n, long long p) {
    long long s = (int32_t)n->v ^ p;
    if (n->l) s += sum_tree(n->l.get(), p);
    if (n->r) s += sum_tree(n->r.get(), p);
    return s;
}

int main() {
    topstack::Stack& s = topstack::plain_stack();
    TNode* root = build(s, DEPTH, 12345);
    long long total = 0;
    for (long long p = 0; p < 8; p++) total += sum_tree(root, p);
    emit(count);
    emit(total);
    return 0;
}
