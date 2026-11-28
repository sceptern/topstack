// Goose's interp benchmark (bench/goose/interp.goose) with Link8<T>: 1-byte links, so a
// number is 5 bytes, an add or multiply 3, a negation 2 (Goose: 5, 9 and 5). A link to
// something more than 64 bytes away goes through a 4-byte anchor made after the node.
#include "bench.h"
#include <topstack/link8.hpp>

#ifndef DEPTH
#define DEPTH BENCH_N
#endif
static const long long M = 1000003;

struct Expr { uint8_t tag; };                                     // 0 num, 1 add, 2 mul, 3 neg
struct Num : Expr { topstack::Packed<int32_t> v; };
struct Bin : Expr { topstack::Link8<Expr> l, r; };
struct Neg : Expr { topstack::Link8<Expr> of; };
static_assert(sizeof(Num) == 5 && sizeof(Bin) == 3 && sizeof(Neg) == 2);

static long long count = 0;

static Expr* build(topstack::Stack& s, int depth, uint64_t seed) {
    uint64_t r = xs_next(seed);
    count++;
    if (depth == 0) return s.make<Num>(Expr{0}, (int32_t)xs_mod(r, 1000));
    uint64_t k = xs_mod(r, 8);
    if (k < 1) {
        Expr* a = build(s, depth - 1, r);
        return s.make<Neg>(Expr{3}, a);
    }
    Expr* a = build(s, depth - 1, r);
    Expr* b = build(s, depth - 1, xs_next(r));
    return s.make<Bin>(Expr{uint8_t(k < 5 ? 1 : 2)}, a, b);
}

static long long eval(const Expr* e, long long p) {
    switch (e->tag) {
        case 0: return ((int32_t)static_cast<const Num*>(e)->v + p) % M;
        case 1: { auto* b = static_cast<const Bin*>(e); return (eval(b->l.get(), p) + eval(b->r.get(), p)) % M; }
        case 2: { auto* b = static_cast<const Bin*>(e); return (eval(b->l.get(), p) * eval(b->r.get(), p)) % M; }
        default: return (M - eval(static_cast<const Neg*>(e)->of.get(), p)) % M;
    }
}

int main() {
    topstack::Stack& s = topstack::plain_stack();
    Expr* root = build(s, DEPTH, 12345);
    long long total = 0;
    for (long long p = 0; p < 8; p++) total = (total + eval(root, p)) % M;
    emit(count);
    emit(total);
    return 0;
}
