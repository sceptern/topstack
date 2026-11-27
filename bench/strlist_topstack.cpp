// Goose's strlist benchmark (bench/goose/strlist.goose) with topstack: split a text
// into an owned list of words, stored the way Goose stores a list of strings, as one
// contiguous block of [length byte][letters] records with no per-word object. The text
// and the list grow in place at the same time, each on its own stack.
#include "bench.h"
#include <topstack/grow_buf.hpp>

#include <cstdlib>
#include <cstring>

using topstack::GrowBuf;

int main() {
    GrowBuf<char> text;
    uint64_t r = 12345;
    for (long long i = 0; i < BENCH_N; i++) {
        r = xs_next(r);
        long long wl = 3 + (long long)xs_mod(r, 18);
        char* p = text.extend((size_t)wl + 1);
        if (!p) std::abort();
        for (long long j = 0; j < wl; j++) {
            r = xs_next(r);
            p[j] = (char)('a' + xs_mod(r, 26));
        }
        p[wl] = ' ';
    }

    // Split: every run of letters becomes a [length][letters] record.
    GrowBuf<unsigned char> words;
    long long count = 0;
    const char* t = text.begin();
    const size_t tl = text.size();
    for (size_t i = 0; i < tl; ) {
        size_t start = i;
        while (i < tl && t[i] != ' ') i++;
        size_t len = i - start;
        if (len > 0) {
            unsigned char* w = words.extend(1 + len);
            if (!w) std::abort();
            w[0] = (unsigned char)len;
            std::memcpy(w + 1, t + start, len);
            count++;
        }
        while (i < tl && t[i] == ' ') i++;
    }

    long long total = 0;
    for (int p = 0; p < 4; p++) {
        for (const unsigned char* w = words.begin(); w < words.end(); w += 1 + w[0])
            total += (long long)w[0] + w[1] + w[w[0]];
    }
    emit(count);
    emit(total);
    return 0;
}
