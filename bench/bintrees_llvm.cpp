// bintrees with LLVM's BumpPtrAllocator (llvm/Support/Allocator.h), the bump
// allocator LLVM and Clang use for their own data structures.
//   VARIANT 0: one allocator per tree, destroyed after it (Reset() is the same).
//   VARIANT 1: one allocator for all short-lived trees, Reset() after each, so it
//              keeps its first slab (Reset frees the rest) for the next tree.
#include "bench.h"
#include <llvm/Support/Allocator.h>
#include <new>
#ifndef MAXDEPTH
#define MAXDEPTH BENCH_N
#endif
static const int MINDEPTH = 4;
struct Node { Node* l; Node* r; };
static Node* make(llvm::BumpPtrAllocator& a, int d) {
    void* p = a.Allocate(sizeof(Node), alignof(Node));
    if (d == 0) return new (p) Node{nullptr, nullptr};
    Node* l = make(a, d - 1); Node* r = make(a, d - 1);
    return new (p) Node{l, r};
}
static long long check(const Node* n) { if (!n->l) return 1; return 1 + check(n->l) + check(n->r); }
#if VARIANT == 1
static llvm::BumpPtrAllocator shared;
#endif
static long long tree_check(int d) {
#if VARIANT == 0
    llvm::BumpPtrAllocator a;
    return check(make(a, d));
#else
    long long c = check(make(shared, d));
    shared.Reset();
    return c;
#endif
}
int main() {
    emit(tree_check(MAXDEPTH + 1));
    llvm::BumpPtrAllocator longa;
    Node* lroot = make(longa, MAXDEPTH);
    for (int d = MINDEPTH; d <= MAXDEPTH; d += 2) {
        long long iters = 1LL << (MAXDEPTH - d + MINDEPTH), sum = 0;
        for (long long i = 0; i < iters; i++) sum += tree_check(d);
        emit(iters); emit(d); emit(sum);
    }
    emit(check(lroot));
}
