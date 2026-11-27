#include <topstack/grow_buf.hpp>
#include <chrono>
#include <cstdio>
#include <vector>
template <class F> static double best_ms(F f) {
    double best = 1e30;
    for (int r = 0; r < 5; ++r) {
        auto t0 = std::chrono::steady_clock::now(); f(); auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}
int main() {
    const int N = 64'000'000;
    long long sink = 0;
    auto [s, err] = topstack::Stack::create(std::size_t(4) << 30);
    double gb = best_ms([&] { topstack::GrowBuf<int> b(*s); for (int i = 0; i < N; ++i) (void)b.push(i); sink += b[N / 2]; });
    double v  = best_ms([&] { std::vector<int> v; for (int i = 0; i < N; ++i) v.push_back(i); sink += v[N / 2]; });
    double vr = best_ms([&] { std::vector<int> v; v.reserve(N); for (int i = 0; i < N; ++i) v.push_back(i); sink += v[N / 2]; });
    std::vector<int> warm; warm.reserve(N);
    double vw = best_ms([&] { warm.clear(); for (int i = 0; i < N; ++i) warm.push_back(i); sink += warm[N / 2]; });
    auto [fresh_s, e2] = topstack::Stack::create(std::size_t(4) << 30);
    double gbcold = 0;
    { auto t0 = std::chrono::steady_clock::now(); topstack::GrowBuf<int> b(*fresh_s); for (int i = 0; i < N; ++i) (void)b.push(i); sink += b[N / 2];
      gbcold = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    std::printf("64M ints, fresh memory:  GrowBuf %.0f ms   vector+reserve %.0f ms   vector (growing) %.0f ms\n", gbcold, vr, v);
    std::printf("64M ints, warm memory:   GrowBuf %.0f ms   vector+reserve %.0f ms        [%lld]\n", gb, vw, sink % 2);
}
