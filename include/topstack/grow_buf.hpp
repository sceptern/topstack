#pragma once
#include <topstack/stack.hpp>

#include <cstdio>
#include <cstdlib>

namespace topstack {

template <class T>
class GrowBuf {
public:
    explicit GrowBuf(Stack& s) : stack_(&s), mark_(s.mark()) {
        s.reclaim_top();
        AllocResult r = s.try_alloc(0, alignof(T));
        char* start = r.ptr ? static_cast<char*>(r.ptr) : s.base_ + s.top_;
        data_ = reinterpret_cast<T*>(start);
        state_ = {start, start};
        if (r.ptr) {
            acquire();
        }
    }

    ~GrowBuf() { release(); }

    GrowBuf(const GrowBuf&) = delete;
    GrowBuf& operator=(const GrowBuf&) = delete;

    GrowBuf(GrowBuf&& other) noexcept
        : stack_(other.stack_), mark_(other.mark_), data_(other.data_), state_(other.state_) {
        take_over_from(other);
    }

    GrowBuf& operator=(GrowBuf&& other) noexcept {
        if (this != &other) {
            release();
            stack_ = other.stack_;
            mark_ = other.mark_;
            data_ = other.data_;
            state_ = other.state_;
            take_over_from(other);
        }
        return *this;
    }

    template <class... Args>
    T* push(Args&&... args) {
        char* next = state_.end + sizeof(T);
        if (next > state_.cap) [[unlikely]] {
            if (!make_room(next)) {
                return nullptr;
            }
        }
        T* obj = new (state_.end) T(std::forward<Args>(args)...);
        state_.end = next;
        return obj;
    }

    T& operator[](std::size_t i)             { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }
    std::size_t size() const                 { return static_cast<std::size_t>(end() - data_); }
    bool empty() const                       { return size() == 0; }
    T* data()                                { return data_; }
    T* begin()                               { return data_; }
    T* end()                                 { return reinterpret_cast<T*>(state_.end); }
    const T* begin() const                   { return data_; }
    const T* end() const                     { return reinterpret_cast<const T*>(state_.end); }

private:
    Stack* stack_;
    Stack::Mark mark_;
    T* data_;
    Stack::GrowState state_;

    void acquire() {
        stack_->grower_ = &state_;
        stack_->alloc_limit_ = 0;
        state_.cap = stack_->base_ + stack_->committed_;
    }

    TOPSTACK_SLOW_PATH bool make_room(char* next) {
        assert(stack_ && "push to a moved-from GrowBuf");
        Stack& s = *stack_;
        if (s.grower_ != &state_) {
            s.reclaim_top();
            char* top = s.base_ + s.top_;
            if (top != state_.end) {
                rule_violation(top > state_.end
                    ? "push to a GrowBuf that is not on top of its stack: something was "
                      "allocated on the same stack after the buffer. Put things that are "
                      "allocated while the buffer grows on a different stack."
                    : "push to a GrowBuf after a rewind freed part of it");
            }
            acquire();
            if (next <= state_.cap) {
                return true;
            }
        }
        std::size_t need = static_cast<std::size_t>(next - s.base_);
        if (need > s.limit_ || !s.commit_through(need)) {
            return false;                         // out of reserved space, or the OS refused
        }
        state_.cap = s.base_ + s.committed_;
        return true;
    }
    void release() {
        if (!stack_) {
            return;
        }
        Stack& s = *stack_;
        s.reclaim_top();
        std::size_t end_offset = static_cast<std::size_t>(state_.end - s.base_);
        if (s.top_ < end_offset) {
            rule_violation("part of a GrowBuf was freed by a rewind while the buffer was still "
                           "alive (it grew after a mark that was later rewound)");
        }
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (T* p = end(); p != data_;) {
                (--p)->~T();
            }
        }
        if (s.top_ == end_offset) {
            s.rewind(mark_);
        }
        stack_ = nullptr;
    }
    void take_over_from(GrowBuf& other) noexcept {
        if (stack_ && stack_->grower_ == &other.state_) {
            stack_->grower_ = &state_;
        }
        char* empty = reinterpret_cast<char*>(other.data_);
        other.stack_ = nullptr;
        other.state_ = {empty, empty};
    }

};

} // namespace topstack
