// Goose's push benchmark with pinned_vector (github.com/mknejp/vmcontainer), the
// container from the Meeting C++ 2018 talk: a vector in reserved virtual memory,
// so it grows in place and pointers to its elements stay valid, like GrowBuf.
// It needs a maximum size up front (here 2 GB, the same as a topstack stack).
#include "bench.h"
#include <vmcontainer/pinned_vector.hpp>
#include <vector>

namespace vmc = mknejp::vmcontainer;
struct Item { int32_t key, val; };

int main() {
    vmc::pinned_vector<Item> items(vmc::max_bytes(std::size_t(2) << 30));
    vmc::pinned_vector<Item*> marks(vmc::max_bytes(std::size_t(2) << 30));
    uint64_t r = 12345;
    for (long long i = 0; i < BENCH_N; i++) {
        r = xs_next(r);
        Item& e = items.emplace_back(Item { (int32_t)xs_mod(r, 1000000), (int32_t)(i % 97) });
        if (i % 64 == 0) marks.push_back(&e);
    }
    long long total = 0, mt = 0;
    for (const Item& it : items) total += it.key;
    for (Item* m : marks) mt += m->val;
    emit((long long)items.size());
    emit((long long)marks.size());
    emit(total);
    emit(mt);
    return 0;
}
