#include <topstack/link8.hpp>
#include <topstack/grow_buf.hpp>

#include <cstdint>
#include <cstdio>
#include <type_traits>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using topstack::Link8;
using topstack::OptLink8;
using topstack::Packed;

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

struct Tree { Packed<std::int32_t> v; OptLink8<Tree> left, right; };
struct Ring { int v; Link8<Ring> next; };

static Tree* build(int depth, int& next) {
    Tree* l = depth ? build(depth - 1, next) : nullptr;
    Tree* r = depth ? build(depth - 1, next) : nullptr;
    return topstack::make<Tree>(next++, l, r);
}
static long sum(const Tree* t) {
    return t ? (std::int32_t)t->v + sum(t->left.get()) + sum(t->right.get()) : 0;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    topstack::Stack& s = topstack::plain_stack();

    // One byte per link, alignment 1: a node of a 4-byte value and two links is 6 bytes.
    CHECK(sizeof(OptLink8<Tree>) == 1 && alignof(OptLink8<Tree>) == 1 && sizeof(Tree) == 6);
    static_assert(!std::is_copy_constructible_v<Link8<Tree>> && !std::is_copy_assignable_v<Link8<Tree>>);
    static_assert(!std::is_constructible_v<Link8<Tree>, std::nullptr_t>);
    static_assert(std::is_constructible_v<OptLink8<Tree>, std::nullptr_t>);

    // Near links: stored directly; null works for OptLink8.
    {
        topstack::Scope sc;
        Tree* a = topstack::make<Tree>(1, nullptr, nullptr);
        Tree* b = topstack::make<Tree>(2, nullptr, nullptr);
        Tree* c = topstack::make<Tree>(3, a, b);
        CHECK(c->left.get() == a && c->right.get() == b && !a->left && !a->right);
        CHECK(c->left->v == 1 && (*c->right).v == 2);
        CHECK(reinterpret_cast<char*>(c) - reinterpret_cast<char*>(b) == 6);   // no anchors
    }

    // A target farther than 64 bytes gets an anchor right after the node.
    {
        topstack::Scope sc;
        Tree* far1 = topstack::make<Tree>(10, nullptr, nullptr);
        (void)s.alloc(1000, 1);
        Tree* near1 = topstack::make<Tree>(11, nullptr, nullptr);
        char* before = static_cast<char*>(s.alloc(0, 1));
        Tree* n = topstack::make<Tree>(12, far1, near1);
        char* after = static_cast<char*>(s.alloc(0, 1));
        CHECK(reinterpret_cast<char*>(n) == before && after - before == 6 + 4);   // node + one anchor
        CHECK(n->left.get() == far1 && n->right.get() == near1 && n->left->v == 10);
        (void)s.alloc(100 << 20, 1);                                            // 100 MB away
        Tree* m = topstack::make<Tree>(13, far1, n);
        CHECK(m->left.get() == far1 && m->right.get() == n && m->right->left->v == 10);
    }

    // A whole tree built bottom-up: every link found again.
    {
        topstack::Scope sc;
        int next = 0;
        Tree* root = build(14, next);
        CHECK(sum(root) == (long)next * (next - 1) / 2);
    }

    // Never-null links, near and far.
    {
        topstack::Scope sc;
        Ring* a = topstack::make<Ring>();                // unset, not read until linked
        (void)a;
        struct Pair { Link8<Ring> x, y; };
        Ring* r1 = topstack::make<Ring>(1, a);
        (void)s.alloc(500, 1);
        Pair* p = topstack::make<Pair>(r1, a);           // both far: two anchors
        CHECK(p->x.get() == r1 && p->y.get() == a && p->x->next.get() == a);
    }

    // Anchors are freed by the Scope together with their nodes.
    {
        auto before = s.mark();
        {
            topstack::Scope sc;
            Tree* a = topstack::make<Tree>(1, nullptr, nullptr);
            (void)s.alloc(4096, 1);
            (void)topstack::make<Tree>(2, a, a);
        }
        CHECK(s.mark().top_ == before.top_);
    }

    // Near links work inside a GrowBuf too.
    {
        topstack::GrowBuf<Tree> buf;
        Tree* a = buf.push(1, nullptr, nullptr);
        Tree* b = buf.push(2, a, nullptr);
        CHECK(b->left.get() == a);
    }

    // The rules. Reaching past 64 bytes needs the node to be the newest allocation.
    CHECK(aborts([] {                                    // a node on the native stack
        topstack::Scope sc;
        Tree* a = topstack::make<Tree>(1, nullptr, nullptr);
        Tree local{2, a, nullptr};
        (void)local;
    }));
    CHECK(aborts([] {                                    // a far link in a GrowBuf element
        topstack::Scope sc;
        Tree* a = topstack::make<Tree>(1, nullptr, nullptr);
        topstack::GrowBuf<Tree> buf(topstack::plain_stack());
        for (int i = 0; i < 20; i++) (void)buf.push(i, nullptr, nullptr);   // 120 bytes
        (void)buf.push(20, a, nullptr);                                       // a is too far
    }));
    CHECK(aborts([] {                                    // a link to another stack
        auto [other, err] = topstack::Stack::create();
        Tree* a = other->make<Tree>(1, nullptr, nullptr);
        (void)topstack::make<Tree>(2, a, nullptr);
    }));
    if (topstack::ref_checks) CHECK(aborts([] {          // reading an unset Link8
        topstack::Scope sc;
        Ring* r = topstack::make<Ring>();
        (void)r->next.get();
    }));
    CHECK(aborts([] {                                    // null in a Link8, through a pointer
        topstack::Scope sc;
        Ring* none = nullptr;
        (void)topstack::make<Ring>(1, none);
    }));

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
