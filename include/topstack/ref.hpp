#pragma once
#include <topstack/stack.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace topstack {

#if !defined(NDEBUG) || defined(TOPSTACK_CHECKED_REFS)
inline constexpr bool ref_checks = true;
#else
inline constexpr bool ref_checks = false;
#endif

// A 4-byte self-relative link: the target's address minus the link's own address.
// Ref<T> is never null, like Goose's T&: reading it is one load and one add. OptRef<T>
// may be null, like Goose's T&?, at the cost of a null check on every read and write.
// Offset 0 means null for an OptRef and "not set yet" for a Ref; a link can never
// point at its own address, so 0 is free for that.
template <class T, bool Nullable>
class BasicRef {
public:
    BasicRef() = default;                          // OptRef: null. Ref: unset until assigned.
    BasicRef(T* target) { set(target); }
    BasicRef(const BasicRef& other) { set(other.get()); }
    BasicRef& operator=(const BasicRef& other) { set(other.get()); return *this; }
    BasicRef& operator=(T* target) { set(target); return *this; }

    // Only an OptRef can be null. For a Ref, writing nullptr doesn't compile.
    BasicRef(std::nullptr_t) requires Nullable {}
    BasicRef& operator=(std::nullptr_t) requires Nullable { store(0); return *this; }
    BasicRef(std::nullptr_t) requires (!Nullable) = delete;
    BasicRef& operator=(std::nullptr_t) requires (!Nullable) = delete;
    explicit operator bool() const requires Nullable { return load() != 0; }

    T* get() const {
        std::int32_t off = load();
        if constexpr (Nullable) {
            if (off == 0) {
                return nullptr;
            }
        } else if constexpr (ref_checks) {
            if (off == 0) [[unlikely]] {
                rule_violation("a Ref was read before it was set");
            }
        }
        return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(this) + off);
    }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }

    friend bool operator==(const BasicRef& a, const BasicRef& b) { return a.get() == b.get(); }
    friend bool operator==(const BasicRef& a, const T* b)        { return a.get() == b; }

    void set(T* target) {
        if constexpr (Nullable) {
            if (!target) {
                store(0);
                return;
            }
        }
        std::intptr_t d = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(target) -
                                                     reinterpret_cast<std::uintptr_t>(this));
        if constexpr (ref_checks) {
            if (!target || static_cast<std::uintptr_t>(d) + 0x80000000u > 0xFFFFFFFFu || d == 0) [[unlikely]] {
                bad_target(target, d);
            }
        }
        store(static_cast<std::int32_t>(d));
    }

private:
    [[noreturn]] TOPSTACK_SLOW_PATH static void bad_target(T* target, std::intptr_t d) {
        rule_violation(!target
            ? "a Ref was set to null; use OptRef for links that can be null"
            : d == 0
            ? "a Ref or OptRef can't point at its own address. This happens when an object's "
              "first field links to the object itself; put another field first."
            : "a Ref or OptRef's target is more than 2 GB away from it: on a different stack, "
              "or the link is in a local variable or temporary (the native stack). Keep "
              "linked objects on the same stack, and build them in place with make or push.");
    }

    std::int32_t load() const { std::int32_t v; std::memcpy(&v, bytes_, 4); return v; }
    void store(std::int32_t v) { std::memcpy(bytes_, &v, 4); }

    unsigned char bytes_[4] = {};
};

template <class T> using Ref = BasicRef<T, false>;
template <class T> using OptRef = BasicRef<T, true>;

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
