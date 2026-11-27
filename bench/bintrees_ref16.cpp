// bintrees with OptRef16<T>: 2-byte links, so a node is 4 bytes (8 with OptRef<T>).
// Uses half the memory of bintrees_ref.cpp, but these trees fit in the CPU's cache,
// so the extra work per link shows and OptRef<T> is faster here. Kept to show both.
#include "bench.h"
#include <topstack/ref16.hpp>
#define MAXDEPTH BENCH_N
static const int MINDEPTH = 4;
struct Node { topstack::OptRef16<Node> l, r; };
static_assert(sizeof(Node) == 4);
static Node* make(topstack::Stack& s, int d) {
    if (d == 0) return s.make<Node>();
    Node* l = make(s, d - 1);
    Node* r = make(s, d - 1);
    return s.make<Node>(l, r);
}
static long long check(const Node* n) { if (!n->l) return 1; return 1 + check(n->l.get()) + check(n->r.get()); }
static long long tree_check(topstack::Stack& s, int d) {
    topstack::Scope sc(s);
    return check(make(s, d));
}
int main() {
    topstack::Stack& s = topstack::plain_stack();
    emit(tree_check(s, MAXDEPTH + 1));
    Node* lroot = make(s, MAXDEPTH);
    for (int d = MINDEPTH; d <= MAXDEPTH; d += 2) {
        long long iters = 1LL << (MAXDEPTH - d + MINDEPTH), sum = 0;
        for (long long i = 0; i < iters; i++) sum += tree_check(s, d);
        emit(iters); emit(d); emit(sum);
    }
    emit(check(lroot));
}
