// Goose's lru benchmark (bench/goose/lru.goose) with topstack: an LRU cache over a
// skewed key stream, with nodes freed and reused in an order that has nothing to do
// with scope. As in Goose's slot_pool, the nodes live in one pool that grows in place
// and a freed node goes on a free list threaded through the freed nodes themselves,
// so there is no separate free-list array. The recency list links are 4-byte
// self-relative NonNullRefs inside the pool: two sentinel nodes mean a list link is
// never null, so following one is a plain add with no null check. The map lives on a different stack, which a OptRef
// cannot reach, so its slots hold 4-byte pool indices (Goose uses pool-relative
// offsets there; the size is the same).
#include "bench.h"
#include <topstack/grow_buf.hpp>
#include <topstack/ref.hpp>

#include <cstdlib>

using topstack::GrowBuf;
using topstack::Ref;

static const long long CAP = BENCH_N / 8;
static const long long KEYS = CAP * 2;

struct Node { int32_t key, val; Ref<Node> prev, next; };   // free nodes: next = next free
struct Slot { int32_t key; int32_t idx; };                    // idx < 0: empty

static GrowBuf<Node>* pool;
static Node* free_head = nullptr;      // the free nodes, chained through next;
static long long nfree = 0;            // the last one's next is never read
static GrowBuf<Slot>* slots;
static uint64_t mask;

static long long home(long long key) {
    return (long long)((((uint64_t)key * 0x9E3779B97F4A7C15ull) >> 24) & mask);
}

// The slot holding `key`, or the empty slot where it would go.
static long long find_slot(long long key) {
    long long i = home(key);
    while ((*slots)[(size_t)i].idx >= 0 && (*slots)[(size_t)i].key != key)
        i = (long long)(((uint64_t)i + 1) & mask);
    return i;
}

// Backward-shift deletion: slide later entries of the probe run down over the
// hole, so no tombstones accumulate.
static void map_remove(long long at) {
    long long i = at, j = at;
    for (;;) {
        j = (long long)(((uint64_t)j + 1) & mask);
        Slot s = (*slots)[(size_t)j];
        if (s.idx < 0) break;
        long long h = home(s.key);
        if ((i < j && (h <= i || h > j)) || (i > j && h <= i && h > j)) { (*slots)[(size_t)i] = s; i = j; }
    }
    (*slots)[(size_t)i].idx = -1;
}

static Node* alloc_node(int32_t key, int32_t val) {
    Node* n;
    if (nfree > 0) {
        n = free_head;
        if (--nfree > 0) free_head = n->next.get();
    } else {
        n = pool->push();
        if (!n) std::abort();
    }
    n->key = key;
    n->val = val;
    return n;
}

static void free_node(Node* n) {
    if (nfree > 0) n->next = free_head;
    free_head = n;
    nfree++;
}

static int32_t index_of(const Node* n) { return (int32_t)(n - pool->data()); }

static void unlink(Node* n) {
    Node* p = n->prev.get();
    Node* q = n->next.get();
    p->next = q;
    q->prev = p;
}

static void link_front(Node* head, Node* n) {
    Node* q = head->next.get();
    n->prev = head;
    n->next = q;
    q->prev = n;
    head->next = n;
}

int main() {
    GrowBuf<Node> nodes;
    GrowBuf<Slot> table;
    pool = &nodes;
    slots = &table;
    long long nslots = 16;
    while (nslots < CAP * 2) nslots *= 2;
    mask = (uint64_t)(nslots - 1);
    for (long long i = 0; i < nslots; i++)
        if (!table.push(0, -1)) std::abort();
    // Two sentinels: the list is never empty, so no link is ever null.
    Node* hi = alloc_node(-1, 0);
    Node* ti = alloc_node(-1, 0);
    hi->next = ti;
    ti->prev = hi;
    long long count = 0, hits = 0, misses = 0, removes = 0;
    uint64_t r = 12345;
    for (long long op = 0; op < BENCH_N; op++) {
        r = xs_next(r);
        long long a = (long long)xs_mod(r, (uint64_t)KEYS);
        long long key = (a * a) / KEYS;                    // Skewed towards small keys.
        long long at = find_slot(key);
        int32_t fi = table[(size_t)at].idx;
        if (xs_mod(r >> 40, 16) == 0) {
            // An invalidation: drop the entry if it is cached.
            if (fi >= 0) {
                Node* f = &nodes[(size_t)fi];
                unlink(f);
                map_remove(at);
                free_node(f);
                count--;
                removes++;
            }
            continue;
        }
        if (fi >= 0) {
            Node* f = &nodes[(size_t)fi];
            f->val += 1;
            unlink(f);
            link_front(hi, f);
            hits++;
            continue;
        }
        misses++;
        long long ins = at;
        if (count == CAP) {
            Node* lru = ti->prev.get();
            unlink(lru);
            map_remove(find_slot(lru->key));
            free_node(lru);
            count--;
            // The deletion may have shifted entries in this key's probe run,
            // so the empty slot found above can be stale.
            ins = find_slot(key);
        }
        Node* n = alloc_node((int32_t)key, (int32_t)(key % 1000));
        table[(size_t)ins] = Slot { (int32_t)key, index_of(n) };
        link_front(hi, n);
        count++;
    }
    // Walk the list from most to least recent, so the order is checked too.
    uint64_t walk = 0;
    long long vals = 0;
    for (const Node* cur = hi->next.get(); cur->key >= 0; cur = cur->next.get()) {
        walk = walk * 31 + (uint64_t)cur->key;
        vals += cur->val;
    }
    emit(hits);
    emit(misses);
    emit(removes);
    emit(count);
    emit_u(walk);
    emit(vals);
    return 0;
}
