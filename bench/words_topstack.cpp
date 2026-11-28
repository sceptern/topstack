// Goose's words benchmark (bench/goose/words.goose) with topstack. As in Goose:
//   - the vocabulary is an array of fixed-capacity words (a length and 20 bytes);
//   - the text grows in place;
//   - the split words are one contiguous block of [length byte][letters] records;
//   - the counts live in an open-addressed table whose keys point at those records,
//     so no key is ever copied or allocated.
// All four grow in place, each on its own stack. The hash is Goose's (see below).
#include "bench.h"
#include <topstack/grow_buf.hpp>

#include <cstdlib>
#include <cstring>

using topstack::GrowBuf;

static const int MAXW = 20;

struct DictWord { uint8_t len; char b[MAXW]; };
struct Slot { const unsigned char* key; int32_t count; };   // key: a [length][letters] record

static inline uint64_t le64(const unsigned char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
static inline uint64_t le32(const unsigned char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

// Goose's own string hash (hash(s: u8[:]) in Goose's stdlib/std.goose), which the
// Goose version uses, for keys of up to 24 bytes (these words are at most 14): 8 bytes
// at a time. Goose's C++ and Rust versions use FNV-1a, one byte at a time.
static uint64_t hash(const unsigned char* s, size_t n) {
    const uint64_t K = 11400714819323198485ull;
    uint64_t h = (uint64_t)n * K, w = 0;
    if (n > 8) {
        size_t i = 0;
        while (i < n - 8) { h = rotl((h ^ le64(s + i)) * K, 29); i += 8; }
        w = le64(s + n - 8);
    } else if (n >= 4) {
        w = le32(s) | le32(s + n - 4) << 32;
    } else if (n > 0) {
        w = s[0] | (uint64_t)s[n >> 1] << 8 | (uint64_t)s[n - 1] << 16;
    }
    h = (h ^ w) * K;
    h = (h ^ (h >> 32)) * 15485907386658061715ull;
    return h ^ (h >> 32);
}

template <class T> static T* need(T* p) { if (!p) std::abort(); return p; }

int main() {
    const long long n = BENCH_N;
    const long long vocab = n / 10 + 16;
    uint64_t r = 12345;

    // A vocabulary of fixed-capacity words, so it stays randomly indexable.
    GrowBuf<DictWord> dict;
    for (long long i = 0; i < vocab; i++) {
        DictWord* w = need(dict.push());
        r = xs_next(r);
        long long len = 3 + (long long)xs_mod(r, 12);
        for (long long j = 0; j < len; j++) { r = xs_next(r); w->b[j] = (char)('a' + xs_mod(r, 26)); }
        w->len = (uint8_t)len;
    }
    // A text of N words drawn with a skew towards the front of the vocabulary.
    GrowBuf<char> text;
    for (long long i = 0; i < n; i++) {
        r = xs_next(r);
        uint64_t a = xs_mod(r, (uint64_t)vocab);
        const DictWord& w = dict[(size_t)((a * a) / (uint64_t)vocab)];
        char* p = need(text.extend(w.len + 1u));
        std::memcpy(p, w.b, w.len);
        p[w.len] = ' ';
    }
    // Split into one contiguous block of [length][letters] records.
    GrowBuf<unsigned char> words;
    long long count = 0;
    const char* t = text.begin();
    const size_t tl = text.size();
    for (size_t i = 0; i < tl; ) {
        size_t start = i;
        while (i < tl && t[i] != ' ') i++;
        size_t len = i - start;
        if (len > 0) {
            unsigned char* w = need(words.extend(1 + len));
            w[0] = (unsigned char)len;
            std::memcpy(w + 1, t + start, len);
            count++;
        }
        while (i < tl && t[i] == ' ') i++;
    }
    // Count, in an open-addressed table whose keys point at the records.
    long long nslots = 16;
    while (nslots < vocab * 4) nslots *= 2;
    const uint64_t mask = (uint64_t)nslots - 1;
    GrowBuf<Slot> slots;
    for (long long i = 0; i < nslots; i++) need(slots.push(nullptr, 0));
    long long distinct = 0;
    for (const unsigned char* w = words.begin(); w < words.end(); w += 1 + w[0]) {
        uint64_t idx = hash(w + 1, w[0]) & mask;
        for (;;) {
            Slot& s = slots[idx];
            if (s.count == 0) { s.key = w; s.count = 1; distinct++; break; }
            if (s.key[0] == w[0] && std::memcmp(s.key + 1, w + 1, w[0]) == 0) { s.count++; break; }
            idx = (idx + 1) & mask;
        }
    }
    long long total = 0;
    for (const Slot& s : slots)
        if (s.count > 0) total += (long long)s.count * ((long long)s.key[0] + s.key[1]);
    emit(count);
    emit(distinct);
    emit(total);
    return 0;
}
