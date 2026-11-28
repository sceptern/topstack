#pragma once
#include <topstack/ref.hpp>

#include <cstddef>
#include <cstdint>

namespace topstack {

template <class T, bool Nullable>
class BasicLink8 {
public:
    BasicLink8() = default;
    BasicLink8(T* target) { set(target); }
    BasicLink8(std::nullptr_t) requires Nullable {}
    BasicLink8(std::nullptr_t) requires (!Nullable) = delete;

    BasicLink8(const BasicLink8&) = delete;
    BasicLink8& operator=(const BasicLink8&) = delete;

    T* get() const {
        unsigned b = byte_;
        if constexpr (Nullable) {
            if (b == 0) {
                return nullptr;
            }
        } else if constexpr (ref_checks) {
            if (b == 0) [[unlikely]] {
                rule_violation("a Link8 was read before it was set");
            }
        }
        const unsigned char* q = reinterpret_cast<const unsigned char*>(this) + (static_cast<std::int8_t>(b) >> 1);
        if ((b & 1) == 0) [[likely]] {
            return non_null(q);
        }
        return reinterpret_cast<const Ref<T>*>(q)->get();
    }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }
    explicit operator bool() const requires Nullable { return byte_ != 0; }

    friend bool operator==(const BasicLink8& a, const T* b) { return a.get() == b; }

private:
    void set(T* target) {
        if (!target) {
            if constexpr (Nullable) {
                byte_ = 0;
                return;
            } else {
                rule_violation("a Link8 was set to null; use OptLink8 for links that can be null");
            }
        }
        std::intptr_t d = reinterpret_cast<const unsigned char*>(target) - reinterpret_cast<const unsigned char*>(this);
        if (static_cast<std::uintptr_t>(d + 64) < 128 && d != 0) [[likely]] {
            byte_ = static_cast<unsigned char>(d * 2);
            return;
        }
        set_far(target);
    }

    TOPSTACK_SLOW_PATH void set_far(T* target) {
        std::uintptr_t self = reinterpret_cast<std::uintptr_t>(this);
        std::uintptr_t block = os::block_of(this);
        if (os::block_of(target) != block || self - block >= relay::max_data) {
            rule_violation("a Link8 and its target must be on the same topstack stack (a link in "
                           "a local variable or temporary is on the native stack). Make the node "
                           "with make, on the stack its targets are on.");
        }
        Stack& s = *static_cast<Stack*>(relay::owner(block));
        if (s.grower_) {
            rule_violation("a Link8 more than 64 bytes from its target can't be in a GrowBuf "
                           "element: its anchor would have to go on the buffer's stack. Make "
                           "such nodes with make instead.");
        }
        std::uintptr_t top = reinterpret_cast<std::uintptr_t>(s.base_) + s.top_;
        if (self >= top || top - self > 63) {
            rule_violation("a Link8 can only reach past 64 bytes while its node is being made: "
                           "it must be in the newest allocation on its stack, at most 63 bytes "
                           "below the top (make the node with make, and keep nodes with links "
                           "small).");
        }
        MakeResult<Ref<T>> anchor = s.try_make<Ref<T>>(target);
        if (!anchor.ptr) {
            rule_violation("out of memory for a Link8 anchor");
        }
        std::intptr_t a = reinterpret_cast<std::uintptr_t>(anchor.ptr) - self;
        byte_ = static_cast<unsigned char>((a * 2) | 1);
    }

    static T* non_null(const unsigned char* t) {
#if defined(__GNUC__) || defined(__clang__)
        if (!t) __builtin_unreachable();
#elif defined(_MSC_VER)
        __assume(t != nullptr);
#endif
        return const_cast<T*>(reinterpret_cast<const T*>(t));
    }

    unsigned char byte_ = 0;
};

template <class T> using Link8 = BasicLink8<T, false>;
template <class T> using OptLink8 = BasicLink8<T, true>;

} // namespace topstack
