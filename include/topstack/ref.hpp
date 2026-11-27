#pragma once
#include <topstack/stack.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace topstack {

#if !defined(NDEBUG) || defined(TOPSTACK_CHECKED_REFS)
inline constexpr bool ref_checks = true;
#else
inline constexpr bool ref_checks = false;
#endif

template <class T>
class Ref {
public:
    Ref() = default;
    Ref(std::nullptr_t) {}
    Ref(T* target) { set(target); }
    Ref(const Ref& other) { set(other.get()); }
    Ref& operator=(const Ref& other) { set(other.get()); return *this; }
    Ref& operator=(T* target) { set(target); return *this; }
    Ref& operator=(std::nullptr_t) { store(0); return *this; }

    T* get() const {
        std::int32_t off = load();
        if (off == 0) {
            return nullptr;
        }
        return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(this) + off);
    }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }
    explicit operator bool() const { return load() != 0; }

    friend bool operator==(const Ref& a, const Ref& b) { return a.get() == b.get(); }
    friend bool operator==(const Ref& a, const T* b)   { return a.get() == b; }

    void set(T* target) {
        if (!target) {
            store(0);
            return;
        }
        std::intptr_t d = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(target) -
                                                     reinterpret_cast<std::uintptr_t>(this));
        if constexpr (ref_checks) {
            if (static_cast<std::uintptr_t>(d) + 0x80000000u > 0xFFFFFFFFu || d == 0) [[unlikely]] {
                bad_target(d);
            }
        }
        store(static_cast<std::int32_t>(d));
    }

private:
    [[noreturn]] TOPSTACK_SLOW_PATH static void bad_target(std::intptr_t d) {
        rule_violation(d == 0
            ? "a Ref can't point at its own address: that offset means null. This happens "
              "when an object's first field links to the object itself; put another field first."
            : "a Ref's target is more than 2 GB away from the Ref: on a different stack, or "
              "the Ref is in a local variable or temporary (the native stack). Keep linked "
              "objects on the same stack, and build them in place with make or push.");
    }

    std::int32_t load() const { std::int32_t v; std::memcpy(&v, bytes_, 4); return v; }
    void store(std::int32_t v) { std::memcpy(bytes_, &v, 4); }

    unsigned char bytes_[4] = {};
};

template <class T>
    requires std::is_trivially_copyable_v<T>
class Packed {
public:
    Packed() = default;
    Packed(T value) { std::memcpy(bytes_, &value, sizeof(T)); }
    Packed& operator=(T value) { std::memcpy(bytes_, &value, sizeof(T)); return *this; }
    operator T() const { T v; std::memcpy(&v, bytes_, sizeof(T)); return v; }

private:
    unsigned char bytes_[sizeof(T)] = {};
};

} // namespace topstack
