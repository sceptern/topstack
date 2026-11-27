// Pushes 64M ints into a GrowBuf and into a std::vector with reserve.
// Each fill loop is its own non-inlined function, so the compiler optimizes both
// the same way a real program's loops would be, instead of specializing one of them.
//   fresh memory: memory that has never been touched (includes page faults and,
//                 for GrowBuf, commit system calls)
//   warm memory:  memory used before, already backed by RAM (the push loop alone)
#include <topstack/grow_buf.hpp>
#include <chrono>
#include <cstdio>
#include <vector>

[[gnu::noinline]] void fill_growbuf(topstack::GrowBuf<int>& b, int n) { for (int i = 0; i < n; ++i) (void)b.push(i); }
[[gnu::noinline]] void fill_vector(std::vector<int>& v, int n)        { for (int i = 0; i < n; ++i) v.push_back(i); }

template <class F> static double time_ms(F f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
template <class F> static double best_ms(F f) {
    double best = 1e30;
    for (int r = 0; r < 7; ++r) best = std::min(best, time_ms(f));
    return best;
}

int main() {
    const int N = 64'000'000;
    long long sink = 0;

    // Fresh memory: one run each, on a new stack and a new vector.
    auto [fresh, e1] = topstack::Stack::create(std::size_t(4) << 30);
    double gb_fresh = time_ms([&] { topstack::GrowBuf<int> b(*fresh); fill_growbuf(b, N); sink += b[N - 1]; });
    double v_fresh  = time_ms([&] { std::vector<int> v; v.reserve(N); fill_vector(v, N); sink += v[N - 1]; });

    // Warm memory: reuse the same stack and the same vector capacity, best of 7.
    auto [warm, e2] = topstack::Stack::create(std::size_t(4) << 30);
    std::vector<int> wv; wv.reserve(N);
    double gb_warm = best_ms([&] { topstack::GrowBuf<int> b(*warm); fill_growbuf(b, N); sink += b[N - 1]; });
    double v_warm  = best_ms([&] { wv.clear(); fill_vector(wv, N); sink += wv[N - 1]; });

    std::printf("64M ints, fresh memory:  GrowBuf %6.1f ms   vector+reserve %6.1f ms\n", gb_fresh, v_fresh);
    std::printf("64M ints, warm memory:   GrowBuf %6.1f ms   vector+reserve %6.1f ms\n", gb_warm, v_warm);
    return int(sink & 1);
}
