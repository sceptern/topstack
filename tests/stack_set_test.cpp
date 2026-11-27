#include <topstack/grow_buf.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using topstack::GrowBuf;
using topstack::StackSet;

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

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // The set lends the lowest free stack, never stack 0, and creates stacks lazily.
    {
        StackSet set;
        CHECK(set.created() == 0 && set.lent() == 0);
        int a = set.take();
        int b = set.take();
        CHECK(a == 1 && b == 2);
        CHECK(set.created() == 2 && set.lent() == 2);
        set.give_back(a);
        CHECK(set.take() == 1);                    // lowest free again
        CHECK(set.created() == 2);                 // reused, not created
        CHECK(set.plain() != nullptr && set.created() == 3);
    }

    // Two buffers growing at the same time, with no stacks named: the push
    // benchmark's shape. Each gets its own stack, so there's no rule violation.
    {
        StackSet& set = topstack::default_set();
        int before = set.lent();
        {
            GrowBuf<int> items;
            GrowBuf<int*> marks;
            CHECK(set.lent() == before + 2);
            for (int i = 0; i < 100'000; ++i) {
                int* e = items.push(i);
                if (i % 64 == 0) (void)marks.push(e);
            }
            CHECK(items.size() == 100'000 && marks.size() == 1563);
            CHECK(*marks[1562] == 1562 * 64);
        }
        CHECK(set.lent() == before);               // both stacks given back
    }

    // A buffer created after another was destroyed reuses its stack, from the start.
    {
        StackSet set;
        int* first;
        {
            GrowBuf<int> a(set);
            first = a.push(1);
        }
        GrowBuf<int> b(set);
        CHECK(b.push(2) == first);
        CHECK(set.created() == 1);
    }

    // Buffers with destructors on lent stacks.
    {
        StackSet set;
        {
            GrowBuf<std::string> names(set);
            for (int i = 0; i < 1000; ++i) (void)names.push(std::string(40, 'x'));
            CHECK(names[999].size() == 40);
        }
        CHECK(set.lent() == 0);
    }

    // Moving a buffer moves the duty to give its stack back: given back once.
    {
        StackSet set;
        {
            GrowBuf<int> a(set);
            (void)a.push(1);
            GrowBuf<int> b(std::move(a));
            CHECK(set.lent() == 1);
            (void)b.push(2);
            CHECK(b.size() == 2);
        }
        CHECK(set.lent() == 0);
        {
            GrowBuf<int> a(set);
            GrowBuf<int> b(set);
            (void)a.push(1);
            (void)b.push(2);
            CHECK(set.lent() == 2);
            a = std::move(b);                      // a's own stack goes back now
            CHECK(set.lent() == 1);
            CHECK(a.size() == 1 && a[0] == 2);
        }
        CHECK(set.lent() == 0);
    }

    // Returning a lent buffer from a function.
    {
        StackSet set;
        auto read = [&set] {
            GrowBuf<int> out(set);
            for (int i = 0; i < 10; ++i) (void)out.push(i);
            return out;
        };
        GrowBuf<int> xs = read();
        (void)xs.push(10);
        CHECK(xs.size() == 11 && xs[10] == 10 && set.lent() == 1);
    }

    // 63 buffers growing at once is fine...
    {
        StackSet set;
        GrowBuf<int>* bufs[63];
        for (auto& b : bufs) b = new GrowBuf<int>(set);
        for (int round = 0; round < 3; ++round)
            for (auto* b : bufs) (void)b->push(round);
        bool ok = true;
        for (auto* b : bufs) ok = ok && b->size() == 3 && (*b)[2] == 2;
        CHECK(ok);
        CHECK(set.lent() == 63);
        for (auto* b : bufs) delete b;
        CHECK(set.lent() == 0);
    }
    // ...a 64th stops the program.
    CHECK(aborts([] {
        StackSet set;
        GrowBuf<int>* bufs[64];
        for (auto& b : bufs) b = new GrowBuf<int>(set);
    }));

    // The OS refusing a stack: the buffer has none, and push returns nullptr.
    {
        StackSet set(std::size_t(2) << 30, std::size_t(1) << 62);   // buffer stacks too big to reserve
        GrowBuf<int> buf(set);
        CHECK(buf.push(1) == nullptr);
        CHECK(buf.size() == 0 && buf.empty());
        CHECK(set.lent() == 0);
    }

    // Stack 0 can be given its own size.
    {
        StackSet set(std::size_t(8) << 30);
        CHECK(set.plain()->reserved() >= (std::size_t(8) << 30));
        int i = set.take();
        CHECK(set.at(i).reserved() < (std::size_t(8) << 30));
    }

    // Each thread has its own default set.
    {
        StackSet* main_set = &topstack::default_set();
        StackSet* other = nullptr;
        std::size_t other_size = 0;
        std::thread t([&] {
            other = &topstack::default_set();
            GrowBuf<int> xs;
            for (int i = 0; i < 1000; ++i) (void)xs.push(i);
            other_size = xs.size();
        });
        t.join();
        CHECK(other != main_set);
        CHECK(other_size == 1000);
    }

    // ---- piece 3: plain allocations with no stack named ----

    // make, make_array and alloc go to stack 0 of the thread's set, and Scope()
    // frees them again, running destructors.
    {
        topstack::Stack& plain = topstack::plain_stack();
        CHECK(&plain == topstack::default_set().plain());
        std::size_t before = plain.used();
        {
            topstack::Scope scope;
            int* x = topstack::make<int>(42);
            double* xs = topstack::make_array<double>(100);
            void* raw = topstack::alloc(64, 64);
            std::string* s = topstack::make<std::string>(100, 'z');
            CHECK(*x == 42 && xs != nullptr && s->size() == 100);
            CHECK(reinterpret_cast<std::uintptr_t>(raw) % 64 == 0);
            CHECK(plain.used() > before);
            auto [p, err] = topstack::try_make<int>(7);
            CHECK(err == topstack::AllocError::Ok && *p == 7);
        }
        CHECK(plain.used() == before);
    }

    // Plain allocations never seal a buffer: buffers live on other stacks.
    {
        GrowBuf<int> xs;
        for (int i = 0; i < 1000; ++i) {
            (void)xs.push(i);
            topstack::Scope scope;
            (void)topstack::make<int>(i);          // would seal xs if they shared a stack
        }
        CHECK(xs.size() == 1000 && xs[999] == 999);
    }

    // Including only stack.hpp is enough for Scope() and make().
    // (This file includes grow_buf.hpp; tests/plain_only.cpp checks stack.hpp alone.)

#ifndef NDEBUG
    // A moved-from buffer still catches pushes in debug builds.
    CHECK(aborts([] {
        StackSet set;
        GrowBuf<int> a(set);
        GrowBuf<int> b(std::move(a));
        (void)a.push(1);
    }));
#endif

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
