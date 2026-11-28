#include <topstack/stack.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using topstack::AllocError;
using topstack::CreateError;
using topstack::Stack;

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) std::printf("PASS  %s\n", #cond);                     \
        else { std::printf("FAIL  %s  (line %d)\n", #cond, __LINE__);   \
               ++failures; }                                            \
    } while (0)

// Runs f in a forked child process and reports whether it died from an assert
// (SIGABRT). This is how we test that bad rewinds are caught. POSIX only.
template <class F>
static bool aborts(F f) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 2);   // hide the assert message
        f();
        _exit(0);           // reaching here means it did NOT abort
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

// A log the cleanup functions append to, so tests can see what ran and in what order.
static int cleanup_log[16];
static int cleanup_count = 0;
static void reset_log() { cleanup_count = 0; }

// A function that allocates inside a Scope and returns early.
static bool returns_early(Stack& s) {
    topstack::Scope scope(s);
    (void)s.alloc(5000);
    if (s.used() > 0) return true;   // early return: the Scope must still rewind
    return false;
}

// Counts how many Trackers are currently alive, to check destructors really run.
struct Tracker {
    static inline int alive = 0;
    int id;
    explicit Tracker(int i) : id(i) { ++alive; }
    ~Tracker() { --alive; }
};

// A type whose constructor always throws.
struct Thrower {
    Thrower() { throw std::runtime_error("constructor failed"); }
    ~Thrower() {}
};

// A plain struct with no destructor: make() should add no note for it.
struct Point { double x, y; };

// A concept: "can make_array<T> be called?" Used to check that the trivial-only
// rule is enforced at compile time.
template <class T>
concept CanMakeArray = requires(Stack& s) { s.make_array<T>(1); };

static_assert(CanMakeArray<int>);
static_assert(CanMakeArray<Point>);
static_assert(!CanMakeArray<std::string>);

static bool is_aligned(const void* p, std::size_t align) {
    return reinterpret_cast<std::uintptr_t>(p) % align == 0;
}

#if defined(__linux__)
// The VmFlags line of the mapping that holds p: "hg" means it asked for huge pages,
// "nh" that it opted out. Empty if p isn't mapped.
static std::string vm_flags(const void* p) {
    std::ifstream f("/proc/self/smaps");
    std::string line;
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    bool inside = false;
    while (std::getline(f, line)) {
        unsigned long lo, hi;
        char dash;
        if (std::sscanf(line.c_str(), "%lx%c%lx", &lo, &dash, &hi) == 3 && dash == '-') {
            inside = a >= lo && a < hi;
        } else if (inside && line.rfind("VmFlags:", 0) == 0) {
            return line + " ";
        }
    }
    return "";
}
static bool has_flag(const std::string& flags, const char* f) {
    return flags.find(std::string(" ") + f + " ") != std::string::npos;
}
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: output survives a crash
    const std::size_t page = topstack::os::page_size();

    // ---------- create ----------
    {
        auto [s, err] = Stack::create();
        CHECK(err == CreateError::Ok);
        CHECK(s != nullptr);
        CHECK(s->reserved() == (std::size_t(2) << 30));
        CHECK(s->used() == 0);
        CHECK(s->committed() == 0);
        CHECK(s->commit_step() == (std::size_t(64) << 10));
    }
    {
        auto [s, err] = Stack::create(1'000'000);
        CHECK(err == CreateError::Ok);
        CHECK(s->reserved() % page == 0);
        CHECK(s->reserved() >= 1'000'000);
        CHECK(s->reserved() < 1'000'000 + page);
    }
    {
        auto [s, err] = Stack::create(1 << 20, 1000);
        CHECK(err == CreateError::Ok);
        CHECK(s->commit_step() == page);
    }
    {
        auto [s, err] = Stack::create(0);
        CHECK(err == CreateError::ZeroSize);
        CHECK(s == nullptr);
    }
    {
        auto [s, err] = Stack::create(1 << 20, 0);
        CHECK(err == CreateError::ZeroCommitStep);
        CHECK(s == nullptr);
    }
    {
        auto [s, err] = Stack::create(SIZE_MAX);
        CHECK(err == CreateError::SizeTooLarge);
        CHECK(s == nullptr);
    }
    {
        auto [s, err] = Stack::create(1 << 20, SIZE_MAX);
        CHECK(err == CreateError::SizeTooLarge);
        CHECK(s == nullptr);
    }
    // A stack holds at most 2 GB (it lives in a 16 GB block with room for relays).
    {
        auto [s, err] = Stack::create((std::size_t(2) << 30) + 1);
        CHECK(err == CreateError::SizeTooLarge);
        CHECK(s == nullptr);
        auto [t, err2] = Stack::create(std::size_t(2) << 30);
        CHECK(err2 == CreateError::Ok && t != nullptr);
    }

    // ---------- alloc ----------

    // Alignment: misalign the top with 1 byte, then ask for each alignment.
    {
        auto [s, err] = Stack::create();
        bool all_aligned = true;
        for (std::size_t align : {1, 2, 4, 8, 16, 64, 4096, 8192}) {
            (void)s->alloc(1, 1);
            void* p = s->alloc(24, align);
            all_aligned &= (p != nullptr && is_aligned(p, align));
        }
        CHECK(all_aligned);
    }

    // Writes work across commit boundaries (a commit bug would segfault here).
    {
        auto [s, err] = Stack::create();
        std::size_t total = 0;
        for (int i = 0; i < 1000; ++i) {
            void* p = s->alloc(1000, 8);
            std::memset(p, 0xAB, 1000);
            total += 1000;
        }
        CHECK(s->used() >= total);
        CHECK(s->committed() >= s->used());
    }

    // The first allocation commits exactly one step.
    {
        auto [s, err] = Stack::create();
        (void)s->alloc(1, 1);
        CHECK(s->committed() == s->commit_step());
    }

    // Allocations don't overlap: consecutive pointers move forward.
    {
        auto [s, err] = Stack::create();
        char* a = static_cast<char*>(s->alloc(100, 1));
        char* b = static_cast<char*>(s->alloc(100, 1));
        CHECK(b >= a + 100);
    }

    // Filling a stack: eventually OutOfReserve, never past the reservation.
    {
        auto [s, err] = Stack::create(1 << 20);
        std::size_t count = 0;
        topstack::AllocResult r;
        while ((r = s->try_alloc(1000, 8)).ptr) {
            std::memset(r.ptr, 0, 1000);
            ++count;
        }
        CHECK(r.error == AllocError::OutOfReserve);
        CHECK(s->used() <= s->reserved());
        CHECK(count == (1 << 20) / 1000);
        CHECK(s->reserved() - s->used() < 1000);   // what is left is too small for another
    }

    // A request bigger than the whole reservation fails cleanly.
    {
        auto [s, err] = Stack::create(1 << 20);
        auto r = s->try_alloc(SIZE_MAX);
        CHECK(r.ptr == nullptr);
        CHECK(r.error == AllocError::OutOfReserve);
        CHECK(s->used() == 0);
    }

    // alloc(0) returns a valid pointer and uses no space.
    {
        auto [s, err] = Stack::create();
        void* p = s->alloc(0, 1);
        CHECK(p != nullptr);
        CHECK(s->used() == 0);
    }

    // A commit step larger than the reservation is capped at the limit.
    {
        auto [s, err] = Stack::create(1 << 20, 1 << 30);
        CHECK(err == CreateError::Ok);
        (void)s->alloc(1, 1);
        CHECK(s->committed() == s->reserved());
    }

    // ---------- mark and rewind ----------

    // Rewinding restores the top, and the freed space is handed out again.
    {
        auto [s, err] = Stack::create();
        (void)s->alloc(100);
        Stack::Mark m = s->mark();
        std::size_t before = s->used();
        void* first = s->alloc(64, 8);
        (void)s->alloc(10'000);
        s->rewind(m);
        CHECK(s->used() == before);
        CHECK(s->alloc(64, 8) == first);
    }

    // Nested marks: rewind the inner one, then the outer one.
    {
        auto [s, err] = Stack::create();
        Stack::Mark outer = s->mark();
        (void)s->alloc(1000);
        std::size_t mid = s->used();
        Stack::Mark inner = s->mark();
        (void)s->alloc(2000);
        s->rewind(inner);
        CHECK(s->used() == mid);
        s->rewind(outer);
        CHECK(s->used() == 0);
    }

    // Rewinding keeps memory committed for reuse.
    {
        auto [s, err] = Stack::create();
        Stack::Mark m = s->mark();
        (void)s->alloc(1 << 20);
        std::size_t committed = s->committed();
        s->rewind(m);
        CHECK(s->committed() == committed);
    }

    // ---------- Scope ----------

    // A Scope frees everything allocated inside it.
    {
        auto [s, err] = Stack::create();
        (void)s->alloc(100);
        std::size_t before = s->used();
        {
            topstack::Scope scope(*s);
            (void)s->alloc(50'000);
            CHECK(s->used() > before);
        }
        CHECK(s->used() == before);
    }

    // ...even when the function returns early from inside it...
    {
        auto [s, err] = Stack::create();
        CHECK(returns_early(*s));
        CHECK(s->used() == 0);
    }

    // ...and when an exception leaves the block.
    {
        auto [s, err] = Stack::create();
        try {
            topstack::Scope scope(*s);
            (void)s->alloc(50'000);
            throw std::runtime_error("boom");
        } catch (const std::runtime_error&) {}
        CHECK(s->used() == 0);
    }

    // ---------- cleanups ----------

    // Cleanups run newest-first.
    {
        auto [s, err] = Stack::create();
        reset_log();
        Stack::Mark m = s->mark();
        s->add_cleanup(nullptr, [](void*) { cleanup_log[cleanup_count++] = 1; });
        s->add_cleanup(nullptr, [](void*) { cleanup_log[cleanup_count++] = 2; });
        s->add_cleanup(nullptr, [](void*) { cleanup_log[cleanup_count++] = 3; });
        s->rewind(m);
        CHECK(cleanup_count == 3);
        CHECK(cleanup_log[0] == 3 && cleanup_log[1] == 2 && cleanup_log[2] == 1);
    }

    // Only cleanups registered after the mark run; older ones wait for their own rewind.
    {
        auto [s, err] = Stack::create();
        reset_log();
        Stack::Mark outer = s->mark();
        s->add_cleanup(nullptr, [](void*) { cleanup_log[cleanup_count++] = 1; });
        Stack::Mark inner = s->mark();
        s->add_cleanup(nullptr, [](void*) { cleanup_log[cleanup_count++] = 2; });
        s->add_cleanup(nullptr, [](void*) { cleanup_log[cleanup_count++] = 3; });
        s->rewind(inner);
        CHECK(cleanup_count == 2);
        s->rewind(inner);                 // nothing new since: runs nothing
        CHECK(cleanup_count == 2);
        s->rewind(outer);
        CHECK(cleanup_count == 3 && cleanup_log[2] == 1);
    }

    // The cleanup receives the object pointer, and sees the object intact:
    // destructors must run before the freed memory is poisoned.
    {
        auto [s, err] = Stack::create();
        reset_log();
        Stack::Mark m = s->mark();
        int* value = static_cast<int*>(s->alloc(sizeof(int), alignof(int)));
        *value = 42;
        s->add_cleanup(value, [](void* p) { cleanup_log[cleanup_count++] = *static_cast<int*>(p); });
        s->rewind(m);
        CHECK(cleanup_count == 1 && cleanup_log[0] == 42);
    }

    // A real std::string on the stack: its destructor frees its heap buffer.
    // Run this file under -fsanitize=address to confirm nothing leaks.
    {
        auto [s, err] = Stack::create();
        {
            topstack::Scope scope(*s);
            void* mem = s->alloc(sizeof(std::string), alignof(std::string));
            auto* str = new (mem) std::string(100, 'x');   // long enough to use the heap
            s->add_cleanup(str, [](void* p) { static_cast<std::string*>(p)->~basic_string(); });
            CHECK(str->size() == 100);
        }
        CHECK(s->used() == 0);
    }

#ifndef NDEBUG
    // Debug builds poison freed memory with 0xDD.
    {
        auto [s, err] = Stack::create();
        Stack::Mark m = s->mark();
        auto* bytes = static_cast<unsigned char*>(s->alloc(64, 1));
        std::memset(bytes, 0x11, 64);
        s->rewind(m);
        bool poisoned = true;
        for (int i = 0; i < 64; ++i) poisoned &= (bytes[i] == 0xDD);
        CHECK(poisoned);
    }

    // Bad rewinds are caught by the asserts (debug builds only).
    CHECK(aborts([] {                           // a stale mark: already rewound past it
        auto [s, err] = Stack::create();
        Stack::Mark outer = s->mark();
        (void)s->alloc(100);
        Stack::Mark inner = s->mark();
        s->rewind(outer);
        s->rewind(inner);
    }));
    CHECK(aborts([] {                           // a mark from a different stack
        auto [a, e1] = Stack::create();
        auto [b, e2] = Stack::create();
        Stack::Mark m = a->mark();
        b->rewind(m);
    }));
#endif

    // ---------- make ----------

    // A trivial type: constructed with its arguments, aligned, and no note added.
    {
        auto [s, err] = Stack::create();
        (void)s->alloc(1, 1);                       // misalign the top
        Point* p = s->make<Point>(1.5, 2.5);
        CHECK(p != nullptr);
        CHECK(is_aligned(p, alignof(Point)));
        CHECK(p->x == 1.5 && p->y == 2.5);
        CHECK(s->note_count() == 0);
    }

    // Constructor arguments are forwarded as given.
    {
        auto [s, err] = Stack::create();
        auto* pair = s->make<std::pair<int, std::string>>(7, "seven");
        CHECK(pair->first == 7 && pair->second == "seven");
    }

    // A type with a destructor gets a note, and rewind destroys it.
    {
        auto [s, err] = Stack::create();
        Tracker::alive = 0;
        {
            topstack::Scope scope(*s);
            Tracker* a = s->make<Tracker>(1);
            Tracker* b = s->make<Tracker>(2);
            CHECK(a->id == 1 && b->id == 2);
            CHECK(Tracker::alive == 2);
            CHECK(s->note_count() == 2);
        }
        CHECK(Tracker::alive == 0);
        CHECK(s->note_count() == 0);
    }

    // A long std::string through make (run under ASan to confirm no leak).
    {
        auto [s, err] = Stack::create();
        topstack::Scope scope(*s);
        std::string* str = s->make<std::string>(200, 'y');
        CHECK(str->size() == 200);
    }

    // If the constructor throws, the exception reaches the caller and no note is added.
    {
        auto [s, err] = Stack::create();
        bool caught = false;
        try {
            (void)s->make<Thrower>();
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught);
        CHECK(s->note_count() == 0);
    }

    // try_make reports why it failed.
    {
        auto [s, err] = Stack::create(4096);
        struct Big { char bytes[100'000]; };
        auto r = s->try_make<Big>();
        CHECK(r.ptr == nullptr);
        CHECK(r.error == AllocError::OutOfReserve);
    }

    // ---------- make_array ----------

    // An array of n elements: aligned, writable, and the right size.
    {
        auto [s, err] = Stack::create();
        (void)s->alloc(1, 1);
        std::size_t before = s->used();
        double* xs = s->make_array<double>(1000);
        CHECK(xs != nullptr);
        CHECK(is_aligned(xs, alignof(double)));
        for (int i = 0; i < 1000; ++i) xs[i] = i * 0.5;
        CHECK(xs[999] == 499.5);
        CHECK(s->used() - before >= 1000 * sizeof(double));
        CHECK(s->note_count() == 0);
    }

    // A count so large that n * sizeof(T) would overflow fails cleanly.
    {
        auto [s, err] = Stack::create();
        auto r = s->try_make_array<double>(SIZE_MAX / 4);
        CHECK(r.ptr == nullptr);
        CHECK(r.error == AllocError::OutOfReserve);
        CHECK(s->used() == 0);
    }

    // An empty array is a valid pointer that uses no space.
    {
        auto [s, err] = Stack::create();
        int* none = s->make_array<int>(0);
        CHECK(none != nullptr);
        CHECK(s->used() == 0);
    }

#if defined(__linux__)
    // Huge pages: a stack asks for them from 2 MB up and opts out below that and in its
    // relay tables, so small stacks stay small. (Checked only where the kernel has them.)
    if (std::ifstream("/sys/kernel/mm/transparent_hugepage/enabled")) {
        auto [big, e1] = Stack::create(std::size_t(1) << 30);
        char* b = static_cast<char*>(big->alloc(0, 1));
        CHECK(has_flag(vm_flags(b), "nh"));                                   // first 2 MB
        CHECK(has_flag(vm_flags(b + (std::size_t(4) << 30)), "nh"));        // relay tables
  #ifndef TOPSTACK_NO_HUGE_PAGES
        CHECK(has_flag(vm_flags(b + (std::size_t(4) << 20)), "hg"));        // beyond 2 MB
  #endif
        auto [small, e2] = Stack::create(std::size_t(1) << 20);
        char* sb = static_cast<char*>(small->alloc(0, 1));
        CHECK(has_flag(vm_flags(sb), "nh") && !has_flag(vm_flags(sb), "hg"));
    }
#endif

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
