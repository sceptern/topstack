#include <topstack/stack.hpp>
#include <cstdint>
#include <cstdio>

using topstack::CreateError;
using topstack::Stack;

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) std::printf("PASS  %s\n", #cond);                     \
        else { std::printf("FAIL  %s  (line %d)\n", #cond, __LINE__);   \
               ++failures; }                                            \
    } while (0)

int main() {
    const std::size_t page = topstack::os::page_size();

    // A normal stack: 2 GB default, nothing used or committed yet.
    {
        auto [s, err] = Stack::create();
        CHECK(err == CreateError::Ok);
        CHECK(s != nullptr);
        CHECK(s->reserved() == (std::size_t(2) << 30));
        CHECK(s->used() == 0);
        CHECK(s->committed() == 0);
        CHECK(s->commit_step() == (std::size_t(64) << 10));
    }

    // Sizes round up to whole pages.
    {
        auto [s, err] = Stack::create(1'000'000);
        CHECK(err == CreateError::Ok);
        CHECK(s->reserved() % page == 0);
        CHECK(s->reserved() >= 1'000'000);
        CHECK(s->reserved() < 1'000'000 + page);
    }

    // Commit steps round up to whole pages too.
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

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
