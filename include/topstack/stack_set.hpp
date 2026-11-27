#pragma once
#include <topstack/stack.hpp>

#include <bit>
#include <cstdint>
#include <memory>

namespace topstack {

class StackSet {
public:
    static constexpr int max_stacks = 64;

    explicit StackSet(std::size_t plain_reserve = std::size_t(2) << 30,
                      std::size_t buffer_reserve = std::size_t(2) << 30)
        : plain_reserve_(plain_reserve), buffer_reserve_(buffer_reserve) {}
    StackSet(const StackSet&) = delete;
    StackSet& operator=(const StackSet&) = delete;

    Stack* plain() { return get(0); }

    int take() {
        std::uint64_t free = ~busy_ & ~std::uint64_t(1);
        if (free == 0) {
            rule_violation("more than 63 GrowBufs are growing at once on one StackSet "
                           "(each needs its own stack). Are buffers being kept alive by mistake?");
        }
        int i = std::countr_zero(free);
        if (!get(i)) {
            return -1;
        }
        busy_ |= std::uint64_t(1) << i;
        return i;
    }

    Stack& at(int i) { return *stacks_[i]; }

    void give_back(int i) {
        assert(i > 0 && i < max_stacks && (busy_ >> i & 1));
        busy_ &= ~(std::uint64_t(1) << i);
    }

    int created() const {
        int n = 0;
        for (const auto& s : stacks_) n += s != nullptr;
        return n;
    }
    int lent() const { return std::popcount(busy_); }

private:
    Stack* get(int i) {
        if (!stacks_[i]) {
            stacks_[i] = Stack::create(i == 0 ? plain_reserve_ : buffer_reserve_).stack;
        }
        return stacks_[i].get();
    }

    std::size_t plain_reserve_;
    std::size_t buffer_reserve_;
    std::unique_ptr<Stack> stacks_[max_stacks];
    std::uint64_t busy_ = 0;
};

namespace detail {
    struct DefaultSetOwner {
        StackSet* set = nullptr;
        ~DefaultSetOwner();
    };
    inline thread_local DefaultSetOwner default_owner;
}

inline StackSet& default_set() {
    StackSet*& set = detail::default_owner.set;
    if (!set) {
        set = new StackSet;
    }
    return *set;
}

namespace detail {
    inline thread_local Stack* plain_cache = nullptr;

    TOPSTACK_SLOW_PATH inline Stack& plain_stack_slow() {
        Stack* s = default_set().plain();
        if (!s) {
            std::fprintf(stderr, "topstack: the OS refused to reserve this thread's plain stack\n");
            std::abort();
        }
        plain_cache = s;
        return *s;
    }
}

// This thread's stack for plain allocations: stack 0 of its default set.
inline Stack& plain_stack() {
    Stack* s = detail::plain_cache;
    if (s) [[likely]] {
        return *s;
    }
    return detail::plain_stack_slow();
}

inline detail::DefaultSetOwner::~DefaultSetOwner() {
    if (set && set->lent() == 0) {
        delete set;
        plain_cache = nullptr;
    }
}

inline Scope::Scope() : Scope(plain_stack()) {}

// Plain allocations on the thread's plain stack, with no stack named.
[[nodiscard]] inline AllocResult try_alloc(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
    return plain_stack().try_alloc(bytes, align);
}
[[nodiscard]] inline void* alloc(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
    return plain_stack().alloc(bytes, align);
}
template <class T, class... Args>
[[nodiscard]] MakeResult<T> try_make(Args&&... args) {
    return plain_stack().try_make<T>(std::forward<Args>(args)...);
}
template <class T, class... Args>
[[nodiscard]] T* make(Args&&... args) {
    return plain_stack().make<T>(std::forward<Args>(args)...);
}
template <class T>
    requires std::is_trivially_destructible_v<T>
[[nodiscard]] MakeResult<T> try_make_array(std::size_t n) {
    return plain_stack().try_make_array<T>(n);
}
template <class T>
    requires std::is_trivially_destructible_v<T>
[[nodiscard]] T* make_array(std::size_t n) {
    return plain_stack().make_array<T>(n);
}

} // namespace topstack
