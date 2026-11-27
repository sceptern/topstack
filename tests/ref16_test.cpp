#include <topstack/ref16.hpp>
#include <topstack/grow_buf.hpp>
#include <cstdio>
#include <type_traits>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
using topstack::OptRef16;
static int failures = 0;
#define CHECK(c) do { if (c) std::printf("PASS  %s\n", #c); else { std::printf("FAIL  %s (line %d)\n", #c, __LINE__); ++failures; } } while (0)
template <class F> static bool aborts(F f) {
    std::fflush(stdout); pid_t pid = fork();
    if (pid == 0) { int d = open("/dev/null", O_WRONLY); dup2(d, 2); f(); _exit(0); }
    int st = 0; waitpid(pid, &st, 0); return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}
struct Node { int v = 0; OptRef16<Node> l = nullptr, r = nullptr; };

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(sizeof(OptRef16<Node>) == 2 && alignof(OptRef16<Node>) == 1);
    topstack::Stack& s = topstack::plain_stack();

    // Near and far links, both directions.
    {
        topstack::Scope sc;
        Node* a = topstack::make<Node>(1);
        (void)s.alloc(100 << 10, 1);                     // 100 KB apart
        Node* b = topstack::make<Node>(2);
        Node* c = topstack::make<Node>(3);
        b->l = c;  b->r = a;                             // near, far backwards
        a->l = b;                                        // far forwards
        CHECK(b->l.get() == c && b->r.get() == a && a->l.get() == b);
        CHECK(a->l->v == 2 && b->r->v == 1);
        Node* d = topstack::make<Node>(4, a, c);         // built in place with a far link
        CHECK(d->l.get() == a && d->r.get() == c);
        b->r = nullptr;
        CHECK(!b->r);
    }

    // The old dangerous case: a far link stored inside a scope into a node made before
    // it. The relay belongs to the old node's region, so the scope doesn't free it.
    {
        topstack::Scope outer;
        Node* oldn = topstack::make<Node>(10);
        (void)s.alloc(200 << 10, 1);
        Node* target = topstack::make<Node>(20);
        for (int i = 0; i < 5; ++i) {
            topstack::Scope inner;
            (void)s.alloc(300 << 10, 1);                 // the scope spans whole regions
            oldn->l = target;                            // far: relay in oldn's region
            Node* tmp = topstack::make<Node>(30);
            tmp->l = oldn;                               // far, in a region the scope frees
        }
        CHECK(oldn->l.get() == target && oldn->l->v == 20);
    }

    // Many scopes, each making a far link in the same region (the one holding the
    // mark), so that region is never cleared: dead relays must get reused.
    {
        topstack::Scope outer;
        Node* far_target = topstack::make<Node>(7);
        (void)s.alloc(100 << 10, 1);
        bool ok = true;
        for (int i = 0; i < 200'000; ++i) {
            topstack::Scope sc;
            Node* n = topstack::make<Node>(i);
            n->l = far_target;
            ok = ok && n->l.get() == far_target;
        }
        CHECK(ok);
    }

    // Overwriting far links again and again in one long-lived node.
    {
        topstack::Scope sc;
        Node* a = topstack::make<Node>(1);
        (void)s.alloc(100 << 10, 1);
        Node* b = topstack::make<Node>(2);
        Node* c = topstack::make<Node>(3);
        bool ok = true;
        for (int i = 0; i < 100'000; ++i) { a->l = (i & 1) ? b : c; ok = ok && a->l.get() == ((i & 1) ? b : c); }
        CHECK(ok);
    }

    // Far links inside a growing GrowBuf: relays don't touch the buffer's stack top.
    {
        topstack::GrowBuf<Node> buf;
        Node* first = buf.push(0);
        for (int i = 1; i < 100'000; ++i) { Node* n = buf.push(i); n->l = first; }
        CHECK(buf.size() == 100'000 && buf[99'999].l.get() == first && buf[99'999].l->v == 0);
    }

    // A far link in a local variable (the native stack) stops the program.
    CHECK(aborts([] {
        topstack::Scope sc;
        Node* target = topstack::make<Node>(1);
        Node local;
        local.l = target;
    }));

    // A region with more than 32 far links uses its big table.
    {
        topstack::Scope sc;
        Node* target = topstack::make<Node>(5);
        (void)topstack::alloc(100 << 10, 1);
        Node* many = topstack::make_array<Node>(1000);   // 1000 nodes in one or two regions
        bool ok = true;
        for (int i = 0; i < 1000; ++i) many[i].l = target;
        for (int i = 0; i < 1000; ++i) ok = ok && many[i].l.get() == target;
        CHECK(ok);
    }

    // Links between different stacks stop the program.
    CHECK(aborts([] {
        topstack::GrowBuf<Node> a, b;
        Node* x = a.push(1); Node* y = b.push(2);
        x->l = y;
    }));

    // Ref16: never null. Same size; near and far links work as for OptRef16.
    {
        using topstack::Ref16;
        CHECK(sizeof(Ref16<Node>) == 2 && alignof(Ref16<Node>) == 1);
        static_assert(!std::is_constructible_v<Ref16<Node>, std::nullptr_t>);
        static_assert(!std::is_assignable_v<Ref16<Node>&, std::nullptr_t>);
        struct Ring { int v; Ref16<Ring> next; };
        topstack::Scope sc;
        Ring* a = topstack::make<Ring>();                // links are set below
        Ring* b = topstack::make<Ring>();
        (void)s.alloc(100 << 10, 1);                     // c is far from b
        Ring* c = topstack::make<Ring>();
        a->v = 1; b->v = 2; c->v = 3;
        a->next = b;                                     // near
        b->next = c;                                     // far, through a relay
        c->next = a;                                     // far, backwards
        int sum = 0;
        Ring* p = a;
        for (int i = 0; i < 6; i++) { sum += p->v; p = p->next.get(); }
        CHECK(sum == 12 && p == a);
    }
    if (topstack::ref_checks) CHECK(aborts([] {
        struct Ring { int v; topstack::Ref16<Ring> next; };
        topstack::Scope sc;
        (void)topstack::make<Ring>()->next.get();        // read before it was set
    }));
    if (topstack::ref_checks) CHECK(aborts([] {
        struct Ring { int v; topstack::Ref16<Ring> next; };
        topstack::Scope sc;
        Ring* none = nullptr;
        topstack::make<Ring>()->next = none;             // null through a pointer
    }));

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
