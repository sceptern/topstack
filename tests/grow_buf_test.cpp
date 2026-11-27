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

    {
        auto [s, err] = Stack::create(4096);
        GrowBuf<std::uint64_t> buf(*s);
        std::size_t pushed = 0;
        while (buf.push(pushed)) ++pushed;
        CHECK(pushed == 4096 / sizeof(std::uint64_t));
        CHECK(buf.size() == pushed);
    }

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

    {
        auto [s1, e1] = Stack::create();
        auto [s2, e2] = Stack::create();
        GrowBuf<int> tokens(*s1), nodes(*s2);
        for (int i = 0; i < 100'000; ++i) { (void)tokens.push(i); (void)nodes.push(-i); }
        CHECK(tokens[777] == 777 && nodes[777] == -777);
    }

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
