#include <topstack/grow_buf.hpp>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using topstack::GrowBuf;
using topstack::Stack;

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

// Records the order destructors run in.
struct Tracker {
    static inline int alive = 0;
    static inline int log[8];
    static inline int logged = 0;
    int id;
    explicit Tracker(int i) : id(i) { ++alive; }
    ~Tracker() { --alive; if (logged < 8) log[logged++] = id; }
};

// Throws from its constructor when asked to.
struct Picky {
    int v;
    explicit Picky(int x) : v(x) { if (x < 0) throw std::runtime_error("negative"); }
};

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Elements never move: pointers taken early stay valid through millions of pushes.
    {
        auto [s, err] = Stack::create();
        GrowBuf<std::uint64_t> buf(*s);
        std::uint64_t* first = buf.push(0);
        for (std::uint64_t i = 1; i < 1000; ++i) (void)buf.push(i);
        std::uint64_t* mid = &buf[999];
        for (std::uint64_t i = 1000; i < 4'000'000; ++i) (void)buf.push(i);
        CHECK(first == &buf[0]);
        CHECK(mid == &buf[999]);
        CHECK(*first == 0 && *mid == 999);
        CHECK(buf.size() == 4'000'000);
        std::uint64_t sum = 0;
        for (std::uint64_t v : buf) sum += v;
        CHECK(sum == 4'000'000ull * 3'999'999ull / 2);
    }

    // Elements are aligned even when the stack's top wasn't.
    {
        auto [s, err] = Stack::create();
        (void)s->alloc(3, 1);
        GrowBuf<double> buf(*s);
        double* p = buf.push(1.5);
        CHECK(reinterpret_cast<std::uintptr_t>(p) % alignof(double) == 0);
        CHECK(*p == 1.5);
    }

    // Destroying the buffer destroys its elements, newest first.
    {
        auto [s, err] = Stack::create();
        Tracker::alive = 0;
        Tracker::logged = 0;
        {
            GrowBuf<Tracker> buf(*s);
            (void)buf.push(1);
            (void)buf.push(2);
            (void)buf.push(3);
            CHECK(Tracker::alive == 3);
        }
        CHECK(Tracker::alive == 0);
        CHECK(Tracker::log[0] == 3 && Tracker::log[1] == 2 && Tracker::log[2] == 1);
    }

    {
        auto [s, err] = Stack::create();
        GrowBuf<std::string> names(*s);
        for (int i = 0; i < 100; ++i) (void)names.push(50, char('a' + i % 26));
        CHECK(names.size() == 100 && names[99].size() == 50);
    }

    // A buffer still on top gives its space back when destroyed.
    {
        auto [s, err] = Stack::create();
        (void)s->alloc(100, 1);
        std::size_t before = s->used();
        {
            GrowBuf<int> buf(*s);
            for (int i = 0; i < 10'000; ++i) (void)buf.push(i);
            CHECK(s->used() > before);
        }
        CHECK(s->used() == before);
    }

    // A buffer no longer on top destroys its elements but leaves the space to the
    // enclosing scope (it can't free memory that something else sits above).
    {
        auto [s, err] = Stack::create();
        Tracker::alive = 0;
        std::size_t before = s->used();
        {
            topstack::Scope scope(*s);
            {
                GrowBuf<Tracker> buf(*s);
                (void)buf.push(1);
                (void)s->make<int>(7);
            }
            CHECK(Tracker::alive == 0);
            CHECK(s->used() > before);
        }
        CHECK(s->used() == before);
    }

    // A constructor that throws doesn't break the buffer: the next push works.
    {
        auto [s, err] = Stack::create();
        GrowBuf<Picky> buf(*s);
        (void)buf.push(1);
        bool caught = false;
        try { (void)buf.push(-1); } catch (const std::runtime_error&) { caught = true; }
        CHECK(caught);
        CHECK(buf.size() == 1);
        Picky* p = buf.push(2);
        CHECK(p == &buf[1] && buf[1].v == 2);
    }

    // Out of space: push returns nullptr.
    {
        auto [s, err] = Stack::create(4096);
        GrowBuf<std::uint64_t> buf(*s);
        std::size_t pushed = 0;
        while (buf.push(pushed)) ++pushed;
        CHECK(pushed == 4096 / sizeof(std::uint64_t));
        CHECK(buf.size() == pushed);
    }

    // extend: adds n elements at once, contiguous with what was there.
    {
        auto [s, err] = Stack::create();
        GrowBuf<unsigned char> buf(*s);
        (void)buf.push(1);
        unsigned char* p = buf.extend(5);
        CHECK(p == &buf[1] && buf.size() == 6);
        for (int i = 0; i < 5; i++) p[i] = (unsigned char)(10 + i);
        CHECK(buf.extend(0) == buf.end() && buf.size() == 6);
        unsigned char* big = buf.extend(200000);          // crosses several commit steps
        CHECK(big == &buf[6] && buf.size() == 200006);
        big[199999] = 7;
        CHECK(buf[0] == 1 && buf[1] == 10 && buf[5] == 14 && buf[200005] == 7);
    }

    // extend out of space returns nullptr and leaves the buffer as it was.
    {
        auto [s, err] = Stack::create(4096);
        GrowBuf<std::uint32_t> buf(*s);
        CHECK(buf.extend(1000) != nullptr && buf.size() == 1000);
        CHECK(buf.extend(25) == nullptr && buf.size() == 1000);
        CHECK(buf.extend(24) != nullptr && buf.size() == 1024);
        CHECK(buf.extend(std::size_t(-1)) == nullptr && buf.size() == 1024);
    }

    // clear: empties the buffer, which then refills from the same place.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        for (int i = 0; i < 1000; i++) (void)buf.push(i);
        int* first = buf.data();
        buf.clear();
        CHECK(buf.empty() && buf.data() == first);
        (void)buf.push(5);
        CHECK(buf.size() == 1 && buf[0] == 5 && &buf[0] == first);
    }

    // clear runs destructors, newest first.
    {
        auto [s, err] = Stack::create();
        Tracker::alive = 0;
        Tracker::logged = 0;
        {
            GrowBuf<Tracker> buf(*s);
            (void)buf.push(1);
            (void)buf.push(2);
            buf.clear();
            CHECK(Tracker::alive == 0 && Tracker::logged == 2);
            CHECK(Tracker::log[0] == 2 && Tracker::log[1] == 1);
            (void)buf.push(3);
        }
        CHECK(Tracker::alive == 0 && Tracker::logged == 3 && Tracker::log[2] == 3);
    }

    // clear after the buffer was sealed by an allocation that has since been freed:
    // the buffer is back on top, so it can be refilled.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        (void)buf.push(1);
        {
            topstack::Scope scope(*s);
            (void)s->make<int>(2);
        }
        buf.clear();
        CHECK(buf.push(3) != nullptr && buf.size() == 1 && buf[0] == 3);
    }

    // clear while something is still allocated after the buffer: pushing is still
    // the "not on top" error.
    CHECK(aborts([] {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        (void)buf.push(1);
        (void)s->make<int>(2);
        buf.clear();
        (void)buf.push(3);
    }));

    // The rule: pushing after something else was allocated on the same stack aborts.
    CHECK(aborts([] {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        (void)buf.push(1);
        (void)s->make<int>(2);
        (void)buf.push(3);
    }));
    CHECK(aborts([] {
        auto [s, err] = Stack::create();
        GrowBuf<int> a(*s);
        (void)a.push(1);
        GrowBuf<int> b(*s);
        (void)b.push(2);
        (void)a.push(3);
    }));

    // Two buffers growing together work fine on two stacks.
    {
        auto [s1, e1] = Stack::create();
        auto [s2, e2] = Stack::create();
        GrowBuf<int> tokens(*s1), nodes(*s2);
        for (int i = 0; i < 100'000; ++i) { (void)tokens.push(i); (void)nodes.push(-i); }
        CHECK(tokens[777] == 777 && nodes[777] == -777);
    }

    // ---------- step 2: the buffer owns the top while it grows ----------

    // The stack sees the growing buffer: used() and mark() include its elements.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        for (int i = 0; i < 1000; ++i) (void)buf.push(i);
        CHECK(s->used() == 1000 * sizeof(int));
        Stack::Mark m = s->mark();
        CHECK(m.top_ == 1000 * sizeof(int));
    }

    // Temporaries above a buffer, once rewound, let it grow again in place.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        for (int i = 0; i < 10; ++i) (void)buf.push(i);
        {
            topstack::Scope temps(*s);
            int* tmp = s->make_array<int>(500);
            tmp[0] = 1;
        }
        for (int i = 10; i < 100'000; ++i) (void)buf.push(i);
        bool ok = true;
        for (int i = 0; i < 100'000; ++i) ok &= (buf[i] == i);
        CHECK(ok);
        CHECK(&buf[10] == &buf[9] + 1);
    }

    // A new buffer seals the older one; once the new one is gone, the old one resumes.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> outer(*s);
        (void)outer.push(1);
        {
            GrowBuf<int> inner(*s);
            for (int i = 0; i < 100; ++i) (void)inner.push(i);
        }
        (void)outer.push(2);
        CHECK(outer.size() == 2 && outer[1] == 2);
        CHECK(s->used() == 2 * sizeof(int));
    }

    // Allocating after a buffer is fine as long as the buffer doesn't grow again.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> tokens(*s);
        for (int i = 0; i < 100; ++i) (void)tokens.push(i);
        int* first = &tokens[0];
        int** refs = s->make_array<int*>(100);
        for (int i = 0; i < 100; ++i) refs[i] = &tokens[i];
        CHECK(refs[99] == first + 99 && *refs[99] == 99);
    }

    CHECK(aborts([] {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        (void)buf.push(1);
        {
            topstack::Scope scope(*s);
            (void)buf.push(2);
        }
    }));

    CHECK(aborts([] {
        auto [s, err] = Stack::create();
        GrowBuf<int> buf(*s);
        (void)buf.push(1);
        {
            topstack::Scope scope(*s);
            (void)buf.push(2);
            (void)s->make<int>(3);
        }
        (void)buf.push(4);
    }));
    // ...or when the buffer is destroyed, before any destructor runs on freed memory.
    CHECK(aborts([] {
        auto [s, err] = Stack::create();
        GrowBuf<std::string> buf(*s);
        (void)buf.push("a");
        {
            topstack::Scope scope(*s);
            (void)buf.push("b");
            (void)s->make<int>(3);
        }
    }));

    // ---- step 4: moving ----

    // Return a growing buffer from a function, then keep pushing to it.
    {
        auto [s, err] = Stack::create();
        auto read_numbers = [](Stack& st) {
            GrowBuf<int> out(st);
            for (int i = 0; i < 1000; ++i) (void)out.push(i);
            return out;
        };
        GrowBuf<int> nums = read_numbers(*s);
        int* first = &nums[0];
        for (int i = 1000; i < 100'000; ++i) (void)nums.push(i);
        CHECK(nums.size() == 100'000 && nums[99'999] == 99'999);
        CHECK(&nums[0] == first);
        CHECK(s->used() == 100'000 * sizeof(int));
    }

    // After a move, the stack follows the new buffer: an alloc seals the new one,
    // and the moved-from one has nothing.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> a(*s);
        (void)a.push(1);
        (void)a.push(2);
        GrowBuf<int> b(std::move(a));
        CHECK(a.size() == 0 && a.empty());
        CHECK(b.size() == 2 && b[1] == 2);
        (void)b.push(3);
        CHECK(s->used() == 3 * sizeof(int));
        int* x = s->make<int>(9);
        CHECK(x == b.end());
    }

    // The moved-from destructor does nothing; the new owner destroys each element once
    // and gives the space back.
    {
        Tracker::alive = 0; Tracker::logged = 0;
        auto [s, err] = Stack::create();
        std::size_t before = s->used();
        {
            GrowBuf<Tracker> a(*s);
            (void)a.push(1);
            (void)a.push(2);
            {
                GrowBuf<Tracker> b(std::move(a));
                CHECK(Tracker::alive == 2);
            }
            CHECK(Tracker::alive == 0);
            CHECK(Tracker::logged == 2 && Tracker::log[0] == 2 && Tracker::log[1] == 1);
            CHECK(s->used() == before);
        }
        CHECK(Tracker::logged == 2);
    }

    // Move assignment: the old elements are destroyed first, then ownership moves.
    {
        Tracker::alive = 0; Tracker::logged = 0;
        auto [s, err] = Stack::create();
        GrowBuf<Tracker> a(*s);
        (void)a.push(1);
        GrowBuf<Tracker> b(*s);
        (void)b.push(2);
        (void)b.push(3);
        a = std::move(b);
        CHECK(Tracker::alive == 2);
        CHECK(Tracker::logged == 1 && Tracker::log[0] == 1);
        CHECK(a.size() == 2 && a[0].id == 2 && b.size() == 0);
        (void)a.push(4);
        CHECK(a.size() == 3 && a[2].id == 4);
    }

    // Moving a sealed buffer works too, and it can resume once it is back on top.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> a(*s);
        (void)a.push(1);
        {
            topstack::Scope scope(*s);
            (void)s->make<int>(5);
        }
        GrowBuf<int> b(std::move(a));
        (void)b.push(2);
        CHECK(b.size() == 2 && b[1] == 2);
    }

    // Self-move-assignment leaves the buffer alone.
    {
        auto [s, err] = Stack::create();
        GrowBuf<int> a(*s);
        (void)a.push(7);
        GrowBuf<int>& alias = a;
        a = std::move(alias);
        CHECK(a.size() == 1 && a[0] == 7);
    }

#ifndef NDEBUG
    // Pushing to a moved-from buffer is a bug: caught in debug builds.
    CHECK(aborts([] {
        auto [s, err] = Stack::create();
        GrowBuf<int> a(*s);
        (void)a.push(1);
        GrowBuf<int> b(std::move(a));
        (void)a.push(2);
    }));
#endif

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
