// Goose's records benchmark (bench/goose/records_var.goose) with topstack: a log of
// variant records stored the way Goose's variable enum stores them. Each record is a
// one-byte tag followed by exactly its own payload, packed with no padding, and a Say's
// text sits inline behind a one-byte length. The log is a GrowBuf of bytes, so it grows
// in place on its own stack: no reallocation, no copy, no doubling.
//
// Record layout (multi-byte fields unaligned, read and written with memcpy):
//   Tick: tag 0
//   Move: tag 1, i32 id, i16 dx, i16 dy             9 bytes
//   Say:  tag 2, i32 id, u8 len, len letters         6 + len bytes (len 4..15)
//   Quit: tag 3, i32 id, u8 code                     6 bytes
//
// One difference from the Goose version: Goose builds each word in a temporary array
// and copies it into the log; here the letters are written straight into the log,
// since the word's length is known before its letters are generated.
#include "bench.h"
#include <topstack/grow_buf.hpp>

#include <cstdlib>
#include <cstring>

using topstack::GrowBuf;

enum : unsigned char { Tick, Move, Say, Quit };

static unsigned char* room(GrowBuf<unsigned char>& log, std::size_t n) {
    unsigned char* p = log.extend(n);
    if (!p) {
        std::fputs("out of memory\n", stderr);
        std::exit(1);
    }
    return p;
}

int main() {
    GrowBuf<unsigned char> log;
    long long count = 0;
    uint64_t r = 12345;
    for (long long i = 0; i < BENCH_N; i++) {
        r = xs_next(r);
        uint64_t k = xs_mod(r, 8);
        int32_t id = (int32_t)xs_mod(r >> 3, 100000);
        if (k < 4) {
            int16_t dx = (int16_t)((long long)xs_mod(r >> 20, 201) - 100);
            int16_t dy = (int16_t)((long long)xs_mod(r >> 40, 201) - 100);
            unsigned char* p = room(log, 9);
            p[0] = Move;
            std::memcpy(p + 1, &id, 4);
            std::memcpy(p + 5, &dx, 2);
            std::memcpy(p + 7, &dy, 2);
        } else if (k < 6) {
            uint64_t w = xs_next(r);                       // the word's own chain
            unsigned n = 4 + (unsigned)xs_mod(w, 12);
            unsigned char* p = room(log, 6 + n);
            p[0] = Say;
            std::memcpy(p + 1, &id, 4);
            p[5] = (unsigned char)n;
            for (unsigned j = 0; j < n; j++) {
                w = xs_next(w);
                p[6 + j] = (unsigned char)('a' + xs_mod(w, 26));
            }
        } else if (k < 7) {
            unsigned char* p = room(log, 6);
            p[0] = Quit;
            std::memcpy(p + 1, &id, 4);
            p[5] = (unsigned char)xs_mod(r >> 20, 256);
        } else {
            *room(log, 1) = Tick;
        }
        count++;
    }

    long long total = 0;
    for (int pass = 0; pass < 4; pass++) {
        const unsigned char* p = log.begin();
        const unsigned char* end = log.end();
        while (p < end) {
            int32_t id;
            switch (p[0]) {
            case Tick:
                total += 1;
                p += 1;
                break;
            case Move: {
                int16_t dx, dy;
                std::memcpy(&id, p + 1, 4);
                std::memcpy(&dx, p + 5, 2);
                std::memcpy(&dy, p + 7, 2);
                total += id + dx + dy;
                p += 9;
                break;
            }
            case Say:
                std::memcpy(&id, p + 1, 4);
                total += id + p[5] + p[6];
                p += 6 + p[5];
                break;
            default:
                std::memcpy(&id, p + 1, 4);
                total += id + p[5];
                p += 6;
                break;
            }
        }
    }
    emit(count);
    emit(total);
    return 0;
}
