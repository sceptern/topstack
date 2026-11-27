#pragma once
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cassert>
#include <bit>
#include <vector>
#include <new>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#if defined(_MSC_VER)
#  define TOPSTACK_SLOW_PATH __declspec(noinline)
#elif defined(__clang__) && defined(__x86_64__)
#  define TOPSTACK_SLOW_PATH [[gnu::noinline, gnu::cold, clang::preserve_all]]
#else
#  define TOPSTACK_SLOW_PATH [[gnu::noinline, gnu::cold]]
#endif

namespace topstack {

namespace os {

inline std::size_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si); return si.dwPageSize;
#else
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
}

// Every stack lives in its own block: block_size bytes of address space starting at
// a multiple of block_size. Any address inside a stack then finds the start of its
// block by masking off the low bits, which is how a Ref16 finds its relay tables.
// Only address space is reserved; memory is committed as parts get used.
inline constexpr std::size_t block_size = std::size_t(16) << 30;

inline std::uintptr_t block_of(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p) & ~(block_size - 1);
}

inline void* reserve_block() {
#if defined(_WIN32)
    for (int attempt = 0; attempt < 16; ++attempt) {
        void* probe = VirtualAlloc(nullptr, 2 * block_size, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return nullptr;
        std::uintptr_t a = (reinterpret_cast<std::uintptr_t>(probe) + block_size - 1) & ~(block_size - 1);
        VirtualFree(probe, 0, MEM_RELEASE);
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), block_size, MEM_RESERVE, PAGE_NOACCESS);
        if (p) return p;
    }
    return nullptr;
#else
    void* p = mmap(nullptr, 2 * block_size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::uintptr_t start = reinterpret_cast<std::uintptr_t>(p);
    std::uintptr_t a = (start + block_size - 1) & ~(block_size - 1);
    if (a > start) munmap(p, a - start);
    std::uintptr_t end = start + 2 * block_size;
    if (end > a + block_size) munmap(reinterpret_cast<void*>(a + block_size), end - (a + block_size));
    return reinterpret_cast<void*>(a);
#endif
}
inline bool commit(void* p, std::size_t bytes) {
#if defined(_WIN32)
    return VirtualAlloc(p, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(p, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

// Return physical pages to the OS and make the range inaccessible again.
inline void decommit(void* p, std::size_t bytes) {
#if defined(_WIN32)
    VirtualFree(p, bytes, MEM_DECOMMIT);
#else
    madvise(p, bytes, MADV_DONTNEED);
    mprotect(p, bytes, PROT_NONE);
#endif
}

inline void release_block(void* p) {
#if defined(_WIN32)
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, block_size);
#endif
}

} // namespace os


namespace relay {
    inline constexpr std::size_t max_data    = std::size_t(2) << 30;
    inline constexpr unsigned    region_bits = 16;
    inline constexpr std::size_t regions     = max_data >> region_bits;
    inline constexpr std::size_t slots       = 32768;
    inline constexpr std::size_t dense_slots = 32;

    struct Entry {
        std::uint32_t target;
        std::uint16_t link_pos;
        std::uint16_t unused;
    };

    inline constexpr std::size_t counts_at = std::size_t(4) << 30;
    inline constexpr std::size_t dense_at  = counts_at + regions * sizeof(std::uint32_t);
    inline constexpr std::size_t big_at    = dense_at + regions * dense_slots * sizeof(Entry);
    inline constexpr std::size_t big_bytes = slots * sizeof(Entry);
    static_assert(big_at + regions * big_bytes <= os::block_size);

    inline std::uint32_t* counts(std::uintptr_t block) {
        return reinterpret_cast<std::uint32_t*>(block + counts_at);
    }
    inline Entry& entry(std::uintptr_t block, std::size_t region, std::size_t i) {
        if (i < dense_slots) {
            return reinterpret_cast<Entry*>(block + dense_at)[region * dense_slots + i];
        }
        return reinterpret_cast<Entry*>(block + big_at + region * big_bytes)[i];
    }
    inline bool commit_big(std::uintptr_t block, std::size_t region) {
        return os::commit(reinterpret_cast<void*>(block + big_at + region * big_bytes), big_bytes);
    }
    inline void freed(char* base, std::size_t new_top, std::size_t old_top) {
        if (old_top <= new_top) return;
        std::size_t first = (new_top + (std::size_t(1) << region_bits) - 1) >> region_bits;
        std::size_t last  = (old_top - 1) >> region_bits;
        std::uint32_t* c = counts(reinterpret_cast<std::uintptr_t>(base));
        for (std::size_t r = first; r <= last; ++r) {
            c[r] = 0;
        }
    }
}

enum class CreateError { Ok, ZeroSize, SizeTooLarge, ZeroCommitStep, ReserveFailed };

class Stack;

struct [[nodiscard]] CreateResult {
    std::unique_ptr<Stack> stack;
    CreateError error;
};

inline const char* to_string(CreateError error) {
    switch (error) {
        case CreateError::Ok:           return "ok";
        case CreateError::ZeroSize: return "stack reservation of size 0!"
                                              "create the stack with a non zero size";
        case CreateError::SizeTooLarge: return "size too large";
        case CreateError::ZeroCommitStep: return "no step";
        case CreateError::ReserveFailed: return "reserve failed";
    }
    return "unknown error";
}

enum class AllocError { Ok, OutOfReserve, CommitFailed };

struct AllocResult {
    void* ptr;
    AllocError error;
};

inline const char* to_string(AllocError error) {
    switch (error) {
        case AllocError::Ok:           return "ok";
        case AllocError::OutOfReserve: return "stack reservation exhausted; "
                                              "create the stack with a larger reserve_bytes";
        case AllocError::CommitFailed: return "commit failed";
    }
    return "unknown error";
}

struct Note {
    void* object;
    void (*destroy)(void*);
};

template <class T>
struct MakeResult {
    T* ptr;
    AllocError error;
};

[[noreturn]] inline void rule_violation(const char* message) {
    std::fprintf(stderr, "topstack rule violation: %s\n", message);
    std::abort();
}

class Stack {
    template <class T> friend class GrowBuf;
    public:
        struct Mark {
            std::size_t top_;
            std::size_t notes;
            const Stack* owner;
        };
    private:
        char* base_;
        std::size_t top_;
        std::size_t committed_;
        std::size_t limit_;
        std::size_t commit_step_;
        static constexpr std::size_t max_commit_jump = std::size_t(64) << 20;
        std::vector<Note> notes_;
    public:
        inline static CreateResult create(std::size_t reserve_bytes = std::size_t(2) << 30, std::size_t commit_step = std::size_t(64) << 10);
        inline ~Stack();
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;

        [[nodiscard]] AllocResult try_alloc(std::size_t bytes, std::size_t align=alignof(std::max_align_t));
        [[nodiscard]] void* alloc(std::size_t bytes, std::size_t align=alignof(std::max_align_t));

        std::size_t used() const { return current_top(); }
        std::size_t committed() const {return committed_;}
        std::size_t reserved() const {return limit_;};
        std::size_t commit_step() const { return commit_step_; }
         [[nodiscard]] Mark mark() const { return {current_top(), notes_.size(), this}; }
        std::size_t note_count() const { return notes_.size(); }
        void add_cleanup(void* object, void (*destroy)(void*)) { notes_.push_back({object, destroy}); }
        inline void rewind(Mark m);
        template <class T, class... Args>
        [[nodiscard]] MakeResult<T> try_make(Args&&... args) {
            AllocResult r = try_alloc(sizeof(T), alignof(T));
            if (!r.ptr) {
                return {nullptr, r.error};
            }

            T* obj = new (r.ptr) T(std::forward<Args>(args)...);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                try {
                    add_cleanup(obj,  [](void* p) { static_cast<T*>(p)->~T(); });
                } catch (...) {
                    obj->~T();
                    throw;
                }
            }
            return {obj, AllocError::Ok};
        }
        template<class T, class... Args>
        [[nodiscard]] T* make(Args&&... args) {
            return try_make<T>(std::forward<Args>(args)...).ptr;
        }
        template<class T>
        requires std::is_trivially_destructible_v<T>
        [[nodiscard]] MakeResult<T> try_make_array(std::size_t n) {
            if (n > SIZE_MAX / sizeof(T)) {
                return {nullptr, AllocError::OutOfReserve};
            }
            AllocResult r = try_alloc(n * sizeof(T), alignof(T));
            if (!r.ptr) {
                return {nullptr, r.error};
            }
            T* first = static_cast<T*>(r.ptr);
            std::uninitialized_default_construct_n(first, n);
            return {first, AllocError::Ok};
        }
        template<class T>
        requires std::is_trivially_destructible_v<T>
        [[nodiscard]] T* make_array(std::size_t n) {
            return try_make_array<T>(n).ptr;
        }
    private:
        inline Stack(char* base, std::size_t reserved, std::size_t step);
        std::size_t current_top() const {
            return grower_ ? static_cast<std::size_t>(grower_->end - base_) : top_;
        }
        inline void reclaim_top() {
            if (grower_) {
                top_ = static_cast<std::size_t>(grower_->end - base_);
                grower_->cap = grower_->end;
                grower_ = nullptr;
                alloc_limit_ = committed_;
            }
        }
        // Commit memory so that offsets up to `end` are usable. end <= limit_.
        bool commit_through(std::size_t end) {
            std::size_t grow = std::min(std::max(committed_, commit_step_), max_commit_jump);
            std::size_t want = std::max(end, committed_ + grow);
            std::size_t new_committed;
            if (want >= limit_ || limit_ - want < commit_step_) {
                new_committed = limit_;
            } else {
                new_committed = (want + commit_step_ - 1) / commit_step_ * commit_step_;
            }
            if (!os::commit(base_ + committed_, new_committed - committed_)) {
                return false;
            }
            committed_ = new_committed;
            if (!grower_) {
                alloc_limit_ = committed_;
            }
            return true;
        }
        TOPSTACK_SLOW_PATH AllocResult try_alloc_slow(std::size_t bytes, std::size_t align) {
            reclaim_top();
            std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(base_) + top_;
            std::uintptr_t aligned = (addr + align - 1) / align * align;
            std::size_t padding = aligned - addr;
            if (padding > limit_ - top_) {
                return {nullptr, AllocError::OutOfReserve};
            }
            std::size_t start = top_ + padding;
            if (bytes > limit_ - start) {
                return {nullptr, AllocError::OutOfReserve};
            }
            std::size_t end = start + bytes;
            if (end > committed_ && !commit_through(end)) {
                return {nullptr, AllocError::CommitFailed};
            }
            top_ = end;
            return {base_ + start, AllocError::Ok};
        }
        struct GrowState {
            char* end;
            char* cap;
        };
        GrowState* grower_ = nullptr;
        std::size_t alloc_limit_ = 0;
};

class Scope {
    private:
        Stack& stack_;
        Stack::Mark mark_;
    public:
        explicit Scope(Stack& s) : stack_(s), mark_(s.mark()) {}
        inline Scope();
        ~Scope() { stack_.rewind(mark_); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
};

CreateResult Stack::create(std::size_t size, std::size_t step) {
    if (step == 0) {
        return {nullptr, CreateError::ZeroCommitStep};
    }
    const std::size_t page = os::page_size();
    if (size == 0) {
        return {nullptr, CreateError::ZeroSize};
    } else if (size > SIZE_MAX - page) {
        return {nullptr, CreateError::SizeTooLarge};
    }
    if (step > SIZE_MAX - page) {
        return {nullptr, CreateError::SizeTooLarge};
    }
    step = (step + page - 1) / page * page;
    size = (size + page - 1) / page * page;
    if (size > (std::size_t(2) << 30)) {
        return {nullptr, CreateError::SizeTooLarge};
    }
    char* base_address = static_cast<char*>(os::reserve_block());
    if (!os::commit(base_address + relay::counts_at, relay::big_at - relay::counts_at)) {
        os::release_block(base_address);
        return {nullptr, CreateError::ReserveFailed};
    }
    if (!base_address) {
        return {nullptr, CreateError::ReserveFailed};
    }
    return {std::unique_ptr<Stack>(new Stack(base_address, size, step)), CreateError::Ok};
}

Stack::Stack(char* base, std::size_t reserved, std::size_t step)
    : base_(base),
      top_(0),
      committed_(0),
      limit_(reserved),
      commit_step_(step)
{

    assert(base_ != nullptr);
    assert(reserved > 0);
}

Stack::~Stack() {
    rewind({0, 0, this});
    os::release_block(base_);
}

inline AllocResult Stack::try_alloc(std::size_t bytes, std::size_t align) {
    assert(std::has_single_bit(align));

    // base_ is page-aligned and pages are at least 4 KB everywhere, so for
    // align <= 4096 rounding the offset gives the same result as rounding the address.
    // And committed_ <= limit_, so fitting below committed_ means fitting at all.
    if (align <= 4096 && bytes <= (std::size_t(1) << 32)) {
        std::size_t start = (top_ + align - 1) & ~(align - 1);
        std::size_t end = start + bytes;
        if (end <= alloc_limit_) [[likely]] {
            top_ = end;
            return {base_ + start, AllocError::Ok};
        }

    }

    return try_alloc_slow(bytes, align);
}

inline void* Stack::alloc(std::size_t bytes, std::size_t align) {
    return try_alloc(bytes, align).ptr;
}

void Stack::rewind(Mark m) {
    assert(m.owner == this);
    if (grower_ && static_cast<std::size_t>(grower_->end - base_) > m.top_) {
        rule_violation("a rewind would free part of a GrowBuf that is still growing: the buffer "
                               "grew after this mark was taken (for example, it was created outside a "
                               "Scope and pushed to inside it). Create the buffer inside the Scope, or "
                               "give the Scope's allocations a different stack.");
    }
    reclaim_top();
    assert(m.top_ <= top_);
    assert(m.notes <= notes_.size());
    while (notes_.size() > m.notes) {
        Note n = notes_.back();
        notes_.pop_back();
        n.destroy(n.object);
    }
    #ifndef NDEBUG
        std::memset(base_ + m.top_, 0xDD, top_ - m.top_);
    #endif
    relay::freed(base_, m.top_, top_);
    top_ = m.top_;
}

} // end of topstack namespace

#include <topstack/stack_set.hpp>
