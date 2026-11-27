// bintrees with the standard C++ arena, std::pmr::monotonic_buffer_resource.
//   VARIANT 0: a new resource per tree, getting its memory from new/delete
//              (how pmr is usually used).
//   VARIANT 1: one buffer, sized once, reused for every tree: each tree's
//              resource starts on the same buffer and never calls new/delete.
#include "bench.h"
#include <memory_resource>
#include <new>
#include <vector>
#ifndef MAXDEPTH
#define MAXDEPTH BENCH_N
#endif
static const int MINDEPTH = 4;
struct Node { Node* l; Node* r; };
static Node* make(std::pmr::memory_resource& m, int d) {
    void* p = m.allocate(sizeof(Node), alignof(Node));
    if (d == 0) return new (p) Node{nullptr, nullptr};
    Node* l = make(m, d - 1); Node* r = make(m, d - 1);
    return new (p) Node{l, r};
}
static long long check(const Node* n) { if (!n->l) return 1; return 1 + check(n->l) + check(n->r); }
#if VARIANT == 1
static std::vector<std::byte> buf(sizeof(Node) << (MAXDEPTH + 2));
#endif
static long long tree_check(int d) {
#if VARIANT == 0
    std::pmr::monotonic_buffer_resource m;
#else
    std::pmr::monotonic_buffer_resource m(buf.data(), buf.size(), std::pmr::null_memory_resource());
#endif
    return check(make(m, d));
}
int main() {
    emit(tree_check(MAXDEPTH + 1));
    std::pmr::monotonic_buffer_resource longm;
    Node* lroot = make(longm, MAXDEPTH);
    for (int d = MINDEPTH; d <= MAXDEPTH; d += 2) {
        long long iters = 1LL << (MAXDEPTH - d + MINDEPTH), sum = 0;
        for (long long i = 0; i < iters; i++) sum += tree_check(d);
        emit(iters); emit(d); emit(sum);
    }
    emit(check(lroot));
}
