#include <topstack/stack.hpp>
#include <cstdio>

int main() {
    auto a = topstack::Stack::create();            // 2 GB default
    auto b = topstack::Stack::create(1'000'000);   // should round to 1,003,520
    auto c = topstack::Stack::create(0);           // should be nullptr
    std::printf("a: %zu\nb: %zu\nc: %s\n",
                a->reserved(), b->reserved(), c ? "not null (bug)" : "nullptr");
}
