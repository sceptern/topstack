#include <topstack/stack.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

static bool is_aligned(const void* p, std::size_t align) {
    return reinterpret_cast<std::uintptr_t>(p) % align == 0;
}

int main() {
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

    // ---------- alloc ----------

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

    {
        auto [s, err] = Stack::create();
        (void)s->alloc(1, 1);
        CHECK(s->committed() == s->commit_step());
    }

    {
        auto [s, err] = Stack::create();
        char* a = static_cast<char*>(s->alloc(100, 1));
        char* b = static_cast<char*>(s->alloc(100, 1));
        CHECK(b >= a + 100);
    }

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
        CHECK(s->reserved() - s->used() < 1000);
    }

    {
        auto [s, err] = Stack::create(1 << 20);
        auto r = s->try_alloc(SIZE_MAX);
        CHECK(r.ptr == nullptr);
        CHECK(r.error == AllocError::OutOfReserve);
        CHECK(s->used() == 0);
    }

    {
        auto [s, err] = Stack::create();
        void* p = s->alloc(0, 1);
        CHECK(p != nullptr);
        CHECK(s->used() == 0);
    }

    {
        auto [s, err] = Stack::create(1 << 20, 1 << 30);
        CHECK(err == CreateError::Ok);
        (void)s->alloc(1, 1);
        CHECK(s->committed() == s->reserved());
    }

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
