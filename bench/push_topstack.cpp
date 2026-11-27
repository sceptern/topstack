// Goose's push benchmark (bench/goose/push.goose) written with topstack.
// The items and the marks grow at the same time, so each gets its own stack
// (only the top of a stack can grow). The marks are real pointers, as in Goose:
// GrowBuf never moves its elements, so no indices are needed.
#include "bench.h"
#include <topstack/grow_buf.hpp>

struct Item { int32_t key, val; };

int main() {
    auto [item_stack, e1] = topstack::Stack::create();
    auto [mark_stack, e2] = topstack::Stack::create();
    topstack::GrowBuf<Item> items(*item_stack);
    topstack::GrowBuf<Item*> marks(*mark_stack);

    uint64_t r = 12345;
    for (long long i = 0; i < BENCH_N; i++) {
        r = xs_next(r);
        Item* e = items.push(Item { (int32_t)xs_mod(r, 1000000), (int32_t)(i % 97) });
        if (i % 64 == 0) (void)marks.push(e);
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
