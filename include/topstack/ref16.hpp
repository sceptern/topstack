#pragma once
#include <topstack/ref.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace topstack {

template <class T, bool Nullable>
class BasicRef16 {
    public:
        BasicRef16() = default;
        BasicRef16(T* target) { set(target); }
        BasicRef16(const BasicRef16& other) { set(other.get()); }
        BasicRef16& operator=(const BasicRef16& other) { set(other.get()); return *this; }
        BasicRef16& operator=(T* target) { set(target); return *this; }

        BasicRef16(std::nullptr_t) requires Nullable {}
        BasicRef16& operator=(std::nullptr_t) requires Nullable { store(0); return *this; }
        BasicRef16(std::nullptr_t) requires (!Nullable) = delete;
        BasicRef16& operator=(std::nullptr_t) requires (!Nullable) = delete;
        explicit operator bool() const requires Nullable { return load() != 0; }

        T* get() const {
            std::int16_t v = load();
            if constexpr (Nullable) {
                if (v == 0) {
                    return nullptr;
                }
            } else if constexpr (ref_checks) {
                if (v == 0) [[unlikely]] {
                    rule_violation("a Ref16 was read before it was set");
                }
            }
            std::uintptr_t self = reinterpret_cast<std::uintptr_t>(this);
            if ((v & 1) == 0) [[likely]] {
                return reinterpret_cast<T*>(self + (std::intptr_t(v) >> 1));
            }
            return far_target(self, std::uint16_t(v) >> 1);
        }
        T* operator->() const { return get(); }
        T& operator*() const { return *get(); }

        friend bool operator==(const BasicRef16& a, const BasicRef16& b) { return a.get() == b.get(); }
        friend bool operator==(const BasicRef16& a, const T* b)          { return a.get() == b; }

        void set(T* target) {
            if constexpr (Nullable) {
                if (!target) {
                    store(0);
                    return;
                }
            } else if constexpr (ref_checks) {
                if (!target) [[unlikely]] {
                    rule_violation("a Ref16 was set to null; use OptRef16 for links that can be null");
                }
            }
            std::intptr_t d = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(target) - reinterpret_cast<std::uintptr_t>(this));
            if (static_cast<std::uintptr_t>(d + 16384) < 32768u) [[likely]] {
                if constexpr (ref_checks) {
                    if (d == 0) {
                        rule_violation("a Ref16 or OptRef16 can't point at its own address. Put "
                                       "another field first.");
                    }
                }
                store(static_cast<std::int16_t>(d * 2));
                return;
            }
            set_far(target);
        }
    private:
        TOPSTACK_SLOW_PATH static T* far_target(std::uintptr_t self, unsigned slot) {
            std::uintptr_t block = os::block_of(reinterpret_cast<void*>(self));
            std::size_t region = (self - block) >> relay::region_bits;
            return reinterpret_cast<T*>(block + relay::entry(block, region, slot).target);
        }

        TOPSTACK_SLOW_PATH void set_far(T* target) {
            std::uintptr_t self = reinterpret_cast<std::uintptr_t>(this);
            std::uintptr_t block = os::block_of(this);
            if (os::block_of(target) != block || self - block >= relay::max_data) {
                rule_violation("a Ref16 or OptRef16 and its target must be on the same topstack stack (a link "
                               "in a local variable or temporary is on the native stack). Keep "
                               "linked objects on one stack, and build them in place.");
            }
            std::size_t region = (self - block) >> relay::region_bits;
            std::uint32_t& count = relay::counts(block)[region];
            unsigned slot;
            if (count < relay::slots) {
                slot = count++;
                if (slot == relay::dense_slots && !relay::commit_big(block, region)) {
                    rule_violation("Ref16: the OS refused memory for a relay table");
                }
            } else {
                slot = find_dead(block, region, self);
            }
            relay::entry(block, region, slot) = {
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target) - block),
                static_cast<std::uint16_t>(self - block),
                0};
            store(far_value(slot));
        }

        static unsigned find_dead(std::uintptr_t block, std::size_t region, std::uintptr_t self) {
            std::uintptr_t region_start = block + (region << relay::region_bits);
            for (unsigned i = 0; i < relay::slots; ++i) {
                const relay::Entry& e = relay::entry(block, region, i);
                std::uintptr_t link = region_start + e.link_pos;
                if (link == self) {
                    return i;
                }
                std::int16_t at;
                std::memcpy(&at, reinterpret_cast<const void*>(link), 2);
                if (at != far_value(i)) {
                    return i;
                }
            }
            rule_violation("Ref16: no free relay slot (this can't happen)");
        }
        static std::int16_t far_value(unsigned slot) { return static_cast<std::int16_t>((slot << 1) | 1); }
        std::int16_t load() const { std::int16_t v; std::memcpy(&v, bytes_, 2); return v; }
        void store(std::int16_t v) { std::memcpy(bytes_, &v, 2); }
        unsigned char bytes_[2] = {};
};

template <class T> using Ref16 = BasicRef16<T, false>;
template <class T> using OptRef16 = BasicRef16<T, true>;

} // namespace topstack
