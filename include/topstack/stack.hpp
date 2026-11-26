#pragma once
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

inline void* reserve(std::size_t bytes) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(nullptr, bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
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

inline void release(void* p, std::size_t bytes) {
#if defined(_WIN32)
    (void)bytes; VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, bytes);
#endif
}

} // namespace os

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

class Stack {
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
        std::vector<Note> notes_;
    public:
        inline static CreateResult create(std::size_t reserve_bytes = std::size_t(2) << 30, std::size_t commit_step = std::size_t(64) << 10);
        inline ~Stack();
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;

        [[nodiscard]] AllocResult try_alloc(std::size_t bytes, std::size_t align=alignof(std::max_align_t));
        [[nodiscard]] void* alloc(std::size_t bytes, std::size_t align=alignof(std::max_align_t));

        std::size_t used() const { return top_;}
        std::size_t committed() const {return committed_;}
        std::size_t reserved() const {return limit_;};
        std::size_t commit_step() const { return commit_step_; }
        [[nodiscard]] Mark mark() const {return {top_, notes_.size(), this}; }
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
        TOPSTACK_SLOW_PATH AllocResult try_alloc_slow(std::size_t bytes, std::size_t align) {
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
            if (end > committed_) {
                std::size_t new_committed;
                if (limit_ - end < commit_step_) {
                    new_committed = limit_;
                } else {
                    new_committed = (end + commit_step_ - 1) / commit_step_ * commit_step_;
                }
                if (!os::commit(base_ + committed_, new_committed - committed_)) {
                    return {nullptr, AllocError::CommitFailed};
                }
                committed_ = new_committed;
            }
            top_ = end;
            return {base_ + start, AllocError::Ok};
        }
};

class Scope {
    private:
        Stack& stack_;
        Stack::Mark mark_;
    public:
        explicit Scope(Stack& s) : stack_(s), mark_(s.mark()) {}
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
    char* base_address = static_cast<char*>(os::reserve(size));
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
    os::release(base_, limit_);
}

inline AllocResult Stack::try_alloc(std::size_t bytes, std::size_t align) {
    assert(std::has_single_bit(align));

    // base_ is page-aligned and pages are at least 4 KB everywhere, so for
    // align <= 4096 rounding the offset gives the same result as rounding the address.
    // And committed_ <= limit_, so fitting below committed_ means fitting at all.
    if (align <= 4096 && bytes <= (std::size_t(1) << 32)) {
        std::size_t start = (top_ + align - 1) & ~(align - 1);
        std::size_t end = start + bytes;
        if (end <= committed_ && end >= start) [[likely]] {
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
    top_ = m.top_;
}

} // end of topstack namespace
