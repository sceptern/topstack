// binary-trees with topstack: a Scope per short-lived tree, nodes from make<T>.
//   VARIANT 0: 16-byte pointer nodes, stack reached through a global pointer.
//   VARIANT 1: 8-byte nodes with 32-bit links, stack through a global pointer.
//   VARIANT 2: like 0, but the stack is passed down the recursion as a parameter.
//   VARIANT 3: like 1, but the stack is passed down the recursion as a parameter.
// Passing the stack as a parameter is what Goose does with its pool.
#include "bench.h"
#include <topstack/stack.hpp>

#ifndef MAXDEPTH
#define MAXDEPTH BENCH_N
#endif
static const int MINDEPTH = 4;

#if VARIANT == 0 || VARIANT == 2
struct Node { Node* l; Node* r; };
#else
struct Node { uint32_t l, r; };   // index from the tree's first node; 0 = null
#endif

#if VARIANT == 0 || VARIANT == 1
static topstack::Stack* S;
#  define STACK_PARAM
#  define STACK_ARG
#  define STK (*S)
#else
#  define STACK_PARAM topstack::Stack& stk,
#  define STACK_ARG stk,
#  define STK stk
#endif

#if VARIANT == 0 || VARIANT == 2

static Node* make(STACK_PARAM int depth) {
    if (depth == 0) return STK.make<Node>(nullptr, nullptr);
    Node* l = make(STACK_ARG depth - 1);
    Node* r = make(STACK_ARG depth - 1);
    return STK.make<Node>(l, r);
}
static long long check(const Node* n) {
    if (!n->l) return 1;
    return 1 + check(n->l) + check(n->r);
}
static long long tree_check(STACK_PARAM int depth) {
    topstack::Scope scope(STK);
    return check(make(STACK_ARG depth));
}
#define LONG_LIVED_ROOT Node* lroot = make(STACK_ARG MAXDEPTH);
#define CHECK_LONG_LIVED check(lroot)

#else

static uint32_t make(STACK_PARAM Node* origin, int depth) {
    if (depth == 0) return uint32_t(STK.make<Node>(0u, 0u) - origin);
    uint32_t l = make(STACK_ARG origin, depth - 1);
    uint32_t r = make(STACK_ARG origin, depth - 1);
    return uint32_t(STK.make<Node>(l, r) - origin);
}
static long long check(const Node* origin, uint32_t i) {
    uint32_t l = origin[i].l;
    if (!l) return 1;
    return 1 + check(origin, l) + check(origin, origin[i].r);
}
static long long tree_check(STACK_PARAM int depth) {
    topstack::Scope scope(STK);
    Node* origin = STK.make<Node>(0u, 0u);   // the sentinel
    return check(origin, make(STACK_ARG origin, depth));
}
#define LONG_LIVED_ROOT Node* lorigin = STK.make<Node>(0u, 0u); uint32_t lroot = make(STACK_ARG lorigin, MAXDEPTH);
#define CHECK_LONG_LIVED check(lorigin, lroot)

#endif

int main() {
    auto [stack, err] = topstack::Stack::create();
#if VARIANT == 0 || VARIANT == 1
    S = stack.get();
#else
    topstack::Stack& stk = *stack;
#endif
    emit(tree_check(STACK_ARG MAXDEPTH + 1));
    LONG_LIVED_ROOT
    for (int d = MINDEPTH; d <= MAXDEPTH; d += 2) {
        long long iters = 1LL << (MAXDEPTH - d + MINDEPTH);
        long long sum = 0;
        for (long long i = 0; i < iters; i++) sum += tree_check(STACK_ARG d);
        emit(iters);
        emit(d);
        emit(sum);
    }
    emit(CHECK_LONG_LIVED);
    return 0;
}
