#include <topstack/ref.hpp>
#include <topstack/grow_buf.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using topstack::OptRef;

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) std::printf("PASS  %s\n", #cond);                     \
        else { std::printf("FAIL  %s  (line %d)\n", #cond, __LINE__);   \
               ++failures; }                                            \
    } while (0)

// Runs f in a child process; true if it stopped with abort() (POSIX only).
template <class F>
static bool aborts(F f) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 2);
        f();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

struct Node { OptRef<Node> l, r; int v = 0; };
struct Tree { OptRef<Node> l, r; };

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    CHECK(sizeof(OptRef<Node>) == 4 && alignof(OptRef<Node>) == 1);
    CHECK(sizeof(Tree) == 8);

    // Packing: a tag next to Refs and Packed fields leaves no padding.
    {
        struct Expr { std::uint8_t tag; };
        struct Bin : Expr { OptRef<Expr> l, r; };
        struct Num : Expr { topstack::Packed<std::int32_t> v; };
        CHECK(sizeof(Bin) == 9 && sizeof(Num) == 5 && alignof(Bin) == 1);

        topstack::Packed<std::int32_t> p = -123456;
        std::int32_t back = p;
        CHECK(back == -123456);
        p = 42;
        CHECK(p + 1 == 43);

        // Nodes made back to back, at odd addresses, still link correctly.
        topstack::Scope scope;
        Num* a = topstack::make<Num>(Expr{0}, 7);
        Num* b = topstack::make<Num>(Expr{0}, 8);
        Bin* c = topstack::make<Bin>(Expr{1}, a, b);
        CHECK(reinterpret_cast<char*>(b) - reinterpret_cast<char*>(a) == 5);
        CHECK(reinterpret_cast<char*>(c) - reinterpret_cast<char*>(b) == 5);
        CHECK(static_cast<Num*>(c->l.get())->v == 7 && static_cast<Num*>(c->r.get())->v == 8);
    }

    // Null by default, from nullptr, and after assigning nullptr.
    {
        OptRef<int> a;
        OptRef<int> b = nullptr;
        CHECK(!a && a.get() == nullptr && !b);
        int x = 5;
        a = &x;
        CHECK(a && a.get() == &x && *a == 5);
        a = nullptr;
        CHECK(!a);
    }

    // Links between objects on a stack: get, ->, *, ==.
    {
        topstack::Scope scope;
        Node* leaf1 = topstack::make<Node>(nullptr, nullptr, 1);
        Node* leaf2 = topstack::make<Node>(nullptr, nullptr, 2);
        Node* root = topstack::make<Node>(leaf1, leaf2, 3);   // Refs built in place from Node*
        CHECK(root->l.get() == leaf1 && root->r.get() == leaf2);
        CHECK(root->l->v == 1 && (*root->r).v == 2);
        CHECK(root->l == leaf1 && !(root->l == root->r));
        CHECK(!leaf1->l && !leaf1->r);
    }

    // Links can point backwards and forwards.
    {
        topstack::Scope scope;
        Node* a = topstack::make<Node>();
        Node* b = topstack::make<Node>();
        a->r = b;     // forwards
        b->l = a;     // backwards
        CHECK(a->r.get() == b && b->l.get() == a);
        CHECK(a->r->l.get() == a);
    }

    // Copying a struct recomputes each link for its new address: same targets.
    {
        topstack::Scope scope;
        Node* t1 = topstack::make<Node>(nullptr, nullptr, 10);
        Node* t2 = topstack::make<Node>(nullptr, nullptr, 20);
        Node* n = topstack::make<Node>(t1, t2, 0);
        Node* copy = topstack::make<Node>(*n);        // copy on the same stack
        CHECK(copy->l.get() == t1 && copy->r.get() == t2);
        Node* again = topstack::make<Node>();
        *again = *copy;                               // assign
        CHECK(again->l.get() == t1 && again->r.get() == t2);
    }

    // But a copy on the native stack (a local variable or a temporary) is too far
    // from the data stack: with checks on, it stops instead of storing a wrong link.
    if (topstack::ref_checks) CHECK(aborts([] {
        topstack::Scope scope;
        Node* t = topstack::make<Node>();
        Node* n = topstack::make<Node>(t, nullptr, 0);
        Node local = *n;
        (void)local;
    }));

    // In a GrowBuf: a linked list built in place, each node pointing to the previous.
    {
        topstack::GrowBuf<Node> list;
        Node* prev = nullptr;
        for (int i = 0; i < 100'000; ++i) {
            Node* n = list.push(prev, nullptr, i);
            prev = n;
        }
        int steps = 0;
        long long sum = 0;
        for (Node* p = prev; p; p = p->l.get()) { sum += p->v; ++steps; }
        CHECK(steps == 100'000 && sum == 100'000ll * 99'999 / 2);
    }

    // Links to things that aren't on a stack work too, if they are close enough.
    {
        int local = 7;
        OptRef<int> r = &local;
        CHECK(*r == 7);
    }

    // Ref: same size and packing as OptRef, and null can't even be written.
    {
        using topstack::Ref;
        CHECK(sizeof(Ref<int>) == 4 && alignof(Ref<int>) == 1);
        static_assert(!std::is_constructible_v<Ref<int>, std::nullptr_t>);
        static_assert(!std::is_assignable_v<Ref<int>&, std::nullptr_t>);

        // A doubly linked list with two sentinels, the shape Ref is for.
        struct Item { int v; Ref<Item> prev, next; };
        topstack::GrowBuf<Item> items;
        Item* head = items.push();
        Item* tail = items.push();
        head->v = tail->v = -1;
        head->next = tail;
        tail->prev = head;
        auto link_front = [&](Item* n) {
            Item* q = head->next.get();
            n->prev = head;
            n->next = q;
            q->prev = n;
            head->next = n;
        };
        for (int i = 1; i <= 3; i++) {
            Item* n = items.push();
            n->v = i;
            link_front(n);
        }
        // Move item 1 (now last) to the front: unlink, then link again.
        Item* one = &items[2];
        one->prev->next = one->next.get();
        one->next->prev = one->prev.get();
        link_front(one);
        std::string order;
        for (Item* c = head->next.get(); c != tail; c = c->next.get()) order += char('0' + c->v);
        CHECK(order == "132");
        std::string back;
        for (Item* c = tail->prev.get(); c != head; c = c->prev.get()) back += char('0' + c->v);
        CHECK(back == "231");

        // Copying a link re-encodes it for its new address.
        items[3].next = head->next;
        CHECK(items[3].next.get() == head->next.get() && items[3].next == one);
    }

    // The remaining checks only exist when ref_checks is on (debug builds, or
    // TOPSTACK_CHECKED_REFS).
    std::printf("ref checks: %s\n", topstack::ref_checks ? "on" : "off");

    // A link to its own address can't be stored: that offset means null.
    if (topstack::ref_checks) CHECK(aborts([] {
        Tree t;
        t.l = reinterpret_cast<Node*>(&t.l);
    }));

    // A target more than 2 GB away can't be stored. (Only the address is used;
    // nothing is read there.)
    if (topstack::ref_checks) CHECK(aborts([] {
        OptRef<int> r;
        int* far = reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(&r) + (std::uintptr_t(3) << 30));
        r = far;
    }));
    if (topstack::ref_checks) CHECK(aborts([] {
        OptRef<int> r;
        int* far = reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(&r) - (std::uintptr_t(3) << 30));
        r = far;
    }));
    // Ref: reading one that was never set, or setting it to null, stops.
    if (topstack::ref_checks) CHECK(aborts([] {
        topstack::GrowBuf<topstack::Ref<int>> links;
        (void)links.push()->get();
    }));
    if (topstack::ref_checks) CHECK(aborts([] {
        topstack::GrowBuf<topstack::Ref<int>> links;
        int* none = nullptr;
        *links.push() = none;
    }));
    if (topstack::ref_checks) CHECK(aborts([] {
        topstack::Ref<int> r;
        r = reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(&r) + (std::uintptr_t(3) << 30));
    }));

    // Just inside the range is fine.
    {
        OptRef<int> r;
        int* edge = reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(&r) + 0x7FFFFFF0u);
        r = edge;
        CHECK(r.get() == edge);
    }

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
