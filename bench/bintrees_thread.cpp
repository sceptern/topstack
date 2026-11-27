// Like bintrees_ideal VARIANT 1, but the top is passed into each call and returned
// from it (in registers), the way Goose threads its stack top through calls.
#include "bench.h"
#include <sys/mman.h>
#include <new>
#define MAXDEPTH BENCH_N
static const int MINDEPTH = 4;
struct Node { uint32_t l, r; };
struct Made { uint32_t idx; char* top; };
static Made make(Node* o, char* top, int d) {
    if (d == 0) { Node* n = new (top) Node{0, 0}; return {uint32_t(n - o), top + sizeof(Node)}; }
    Made l = make(o, top, d - 1);
    Made r = make(o, l.top, d - 1);
    Node* n = new (r.top) Node{l.idx, r.idx};
    return {uint32_t(n - o), r.top + sizeof(Node)};
}
static long long check(const Node* o, uint32_t i) { uint32_t l = o[i].l; if (!l) return 1; return 1 + check(o, l) + check(o, o[i].r); }
static char* top;
static long long tree_check(int d) {
    Node* o = new (top) Node{0, 0};
    Made m = make(o, top + sizeof(Node), d);
    return check(o, m.idx);          // top is unchanged: the tree is gone after this
}
int main() {
    top = (char*)mmap(nullptr, size_t(2) << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    emit(tree_check(MAXDEPTH + 1));
    Node* lo = new (top) Node{0, 0};
    Made lr = make(lo, top + sizeof(Node), MAXDEPTH); top = lr.top;
    for (int d = MINDEPTH; d <= MAXDEPTH; d += 2) {
        long long iters = 1LL << (MAXDEPTH - d + MINDEPTH), sum = 0;
        for (long long i = 0; i < iters; i++) sum += tree_check(d);
        emit(iters); emit(d); emit(sum);
    }
    emit(check(lo, lr.idx));
}
