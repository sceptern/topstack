// bintrees with Google's protobuf Arena (google::protobuf::Arena), which can hold any
// C++ type through Arena::Create. Nodes are 16 bytes with ordinary pointers.
//   VARIANT 0: a new Arena per tree.
//   VARIANT 1: one Arena for the short-lived trees, Reset() after each tree.
//   VARIANT 2: like 1, but the Arena starts on one big buffer allocated once and
//              reused for every tree (ArenaOptions::initial_block), so trees never
//              need malloc.
#include "bench.h"
#include <google/protobuf/arena.h>
#include <vector>

#ifndef MAXDEPTH
#define MAXDEPTH BENCH_N
#endif
static const int MINDEPTH = 4;
using google::protobuf::Arena;

struct Node { Node* l; Node* r; };
static Node* make(Arena& a, int d) {
    if (d == 0) return Arena::Create<Node>(&a, Node{nullptr, nullptr});
    Node* l = make(a, d - 1);
    Node* r = make(a, d - 1);
    return Arena::Create<Node>(&a, Node{l, r});
}
static long long check(const Node* n) { if (!n->l) return 1; return 1 + check(n->l) + check(n->r); }

#if VARIANT == 2
static std::vector<char> buffer(sizeof(Node) << (MAXDEPTH + 2));
static Arena& shared() {
    static Arena* a = [] {
        google::protobuf::ArenaOptions o;
        o.initial_block = buffer.data();
        o.initial_block_size = buffer.size();
        return new Arena(o);
    }();
    return *a;
}
#elif VARIANT == 1
static Arena& shared() { static Arena a; return a; }
#endif

static long long tree_check(int d) {
#if VARIANT == 0
    Arena a;
    return check(make(a, d));
#else
    Arena& a = shared();
    long long c = check(make(a, d));
    a.Reset();
    return c;
#endif
}

int main() {
    emit(tree_check(MAXDEPTH + 1));
    Arena long_arena;
    Node* lroot = make(long_arena, MAXDEPTH);
    for (int d = MINDEPTH; d <= MAXDEPTH; d += 2) {
        long long iters = 1LL << (MAXDEPTH - d + MINDEPTH), sum = 0;
        for (long long i = 0; i < iters; i++) sum += tree_check(d);
        emit(iters); emit(d); emit(sum);
    }
    emit(check(lroot));
}
