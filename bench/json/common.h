// Shared parts of the JSON benchmarks (the .cpp files in this folder): reading the input, the
// checksums every version prints, and cJSON's own parsing and printing of numbers and
// strings, so the topstack ports read and print byte for byte what cJSON does.
//
// Each program reads JSON_FILE (default json/data/twitter.json, from bench) and, JSON_ROUNDS times,
// parses it, walks the tree, prints it (unless built with -DPRINT=0) and frees it.
// It prints the walk's checksums (the same for every library) and then the printed
// text's length and hash (the same for cJSON and its ports).
#pragma once
#include <bit>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <charconv>
#include <system_error>

#ifndef PRINT
#define PRINT 1
#endif
// FAST=1: the same results, by faster means (used by the *_fast builds of the ports):
// std::from_chars and std::to_chars for numbers instead of strtod, sprintf and sscanf,
// and 8 bytes at a time when looking for whitespace or a string's end.
#ifndef FAST
#define FAST 0
#endif

inline std::string read_input() {
    const char* path = std::getenv("JSON_FILE");
    if (!path) path = "json/data/twitter.json";
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(static_cast<size_t>(n), '\0');
    if (std::fread(s.data(), 1, s.size(), f) != s.size()) { std::perror(path); std::exit(1); }
    std::fclose(f);
    return s;                                   // std::string keeps a '\0' after the data
}

inline long rounds() {
    const char* r = std::getenv("JSON_ROUNDS");
    return r ? std::atol(r) : 10;
}

// What a walk of the tree adds up. Order doesn't matter, so a tree whose children are
// linked last to first can be walked as it is.
struct Walk {
    long long values = 0;       // every value, including object members and containers
    long long bytes = 0;        // string bytes, keys and values
    long long firsts = 0;       // sum of each string's first byte
    uint64_t number_bits = 0;   // sum of every number's bit pattern
    void string(const unsigned char* p, size_t n) { bytes += (long long)n; if (n) firsts += p[0]; }
    void number(double d) { uint64_t b; std::memcpy(&b, &d, 8); number_bits += b; }
};

inline uint64_t hash_text(const char* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ (unsigned char)p[i]) * 0x100000001b3ull;
    return h;
}

inline void report(const Walk& w, size_t printed, uint64_t hash) {
    std::printf("values %lld\nstring bytes %lld\nfirst bytes %lld\nnumber bits %llu\n",
                w.values, w.bytes, w.firsts, (unsigned long long)w.number_bits);
    if (PRINT) std::printf("printed %zu\nprint hash %llu\n", printed, (unsigned long long)hash);
}

// --- cJSON's printing, for the ports ---------------------------------------------

// cJSON's compare_double.
inline bool cjson_same_double(double a, double b) {
    double most = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return std::fabs(a - b) <= most * DBL_EPSILON;
}

// cJSON's print_number: an integer if the double equals its int (cJSON keeps the int
// saturated to INT_MIN..INT_MAX), else 15 digits if they read back, else 17.
inline int cjson_number_text(double d, char* buf /* 26 bytes */) {
    if (std::isnan(d) || std::isinf(d)) return std::sprintf(buf, "null");
    int i = d >= INT_MAX ? INT_MAX : d <= (double)INT_MIN ? INT_MIN : (int)d;
#if FAST
    // The same text, through to_chars ("as if by printf") and from_chars (exact, as sscanf).
    if (d == (double)i) return (int)(std::to_chars(buf, buf + 25, i).ptr - buf);
    {
        char* e = std::to_chars(buf, buf + 25, d, std::chars_format::general, 15).ptr;
        double test = 0;
        auto r = std::from_chars(buf, e, test);
        // Out of range: sscanf gives +-inf (or 0), and cJSON's check then accepts an
        // infinity for DBL_MAX, so it keeps the 15 digits. Do the same.
        if (r.ec == std::errc::result_out_of_range)
            test = std::fabs(d) > 1 ? std::copysign(HUGE_VAL, d) : 0.0;
        else if (r.ec != std::errc())
            test = 0;
        if (!cjson_same_double(test, d))
            e = std::to_chars(buf, buf + 25, d, std::chars_format::general, 17).ptr;
        *e = '\0';
        return (int)(e - buf);
    }
#endif
    if (d == (double)i) return std::sprintf(buf, "%d", i);
    int len = std::sprintf(buf, "%1.15g", d);
    double test = 0;
    if (std::sscanf(buf, "%lg", &test) != 1 || !cjson_same_double(test, d))
        len = std::sprintf(buf, "%1.17g", d);
    return len;
}

// Out needs char* extend(size_t n): room for n more bytes at the end.
template <class Out>
inline void cjson_put_number(Out& out, double d) {
    char buf[26];
    int n = cjson_number_text(d, buf);
    std::memcpy(out.extend((size_t)n), buf, (size_t)n);
}

// cJSON's print_string_ptr: quotes, and escapes for '"', '\\' and control characters.
template <class Out>
inline void cjson_put_string(Out& out, const unsigned char* s, size_t n) {
    size_t extra = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') extra += 1;
        else if (c < 32) extra += 5;
    }
    char* o = out.extend(n + extra + 2);
    *o++ = '"';
    if (extra == 0) {
        std::memcpy(o, s, n);
        o[n] = '"';
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c > 31 && c != '"' && c != '\\') { *o++ = (char)c; continue; }
        *o++ = '\\';
        switch (c) {
            case '\\': *o++ = '\\'; break;
            case '"':  *o++ = '"'; break;
            case '\b': *o++ = 'b'; break;
            case '\f': *o++ = 'f'; break;
            case '\n': *o++ = 'n'; break;
            case '\r': *o++ = 'r'; break;
            case '\t': *o++ = 't'; break;
            default: {
                char tmp[6];
                std::snprintf(tmp, sizeof tmp, "u%04x", c);
                std::memcpy(o, tmp, 5);
                o += 5;
            }
        }
    }
    *o = '"';
}

// --- cJSON's string and number parsing, for the ports -----------------------------

inline unsigned cjson_hex4(const unsigned char* in) {
    unsigned h = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = in[i];
        if (c >= '0' && c <= '9') h += c - '0';
        else if (c >= 'A' && c <= 'F') h += 10u + c - 'A';
        else if (c >= 'a' && c <= 'f') h += 10u + c - 'a';
        else return 0;
        if (i < 3) h <<= 4;
    }
    return h;
}

// Decodes the string body [p, end) (escapes included, quotes excluded) into out, which
// must have room for end - p bytes. Returns the decoded length, or -1 if cJSON would
// reject it. Like cJSON, a decoded string ends at its first '\0'.
inline long cjson_decode_string(const unsigned char* p, const unsigned char* end, unsigned char* out) {
    unsigned char* o = out;
    while (p < end) {
        if (*p != '\\') { *o++ = *p++; continue; }
        int seq = 2;
        switch (p[1]) {
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'n': *o++ = '\n'; break;
            case 'r': *o++ = '\r'; break;
            case 't': *o++ = '\t'; break;
            case '"': case '\\': case '/': *o++ = p[1]; break;
            case 'u': {
                if (end - p < 6) return -1;
                unsigned long cp;
                unsigned first = cjson_hex4(p + 2);
                if (first >= 0xDC00 && first <= 0xDFFF) return -1;
                if (first >= 0xD800 && first <= 0xDBFF) {
                    const unsigned char* q = p + 6;
                    if (end - q < 6 || q[0] != '\\' || q[1] != 'u') return -1;
                    unsigned second = cjson_hex4(q + 2);
                    if (second < 0xDC00 || second > 0xDFFF) return -1;
                    cp = 0x10000 + (((first & 0x3FF) << 10) | (second & 0x3FF));
                    seq = 12;
                } else {
                    cp = first;
                    seq = 6;
                }
                int len; unsigned char mark;
                if (cp < 0x80) { len = 1; mark = 0; }
                else if (cp < 0x800) { len = 2; mark = 0xC0; }
                else if (cp < 0x10000) { len = 3; mark = 0xE0; }
                else if (cp <= 0x10FFFF) { len = 4; mark = 0xF0; }
                else return -1;
                for (int k = len - 1; k > 0; k--) { o[k] = (unsigned char)((cp | 0x80) & 0xBF); cp >>= 6; }
                o[0] = len > 1 ? (unsigned char)((cp | mark) & 0xFF) : (unsigned char)(cp & 0x7F);
                o += len;
                break;
            }
            default: return -1;
        }
        p += seq;
    }
    void* nul = std::memchr(out, 0, (size_t)(o - out));
    return nul ? (unsigned char*)nul - out : o - out;
}

// cJSON's parse_number: take the run of number characters, then strtod. Returns the
// number of bytes read (0: not a number). With FAST, short integers are read directly
// and the rest with std::from_chars, which rounds exactly like strtod; strtod is kept
// for numbers out of range (from_chars reports those instead of giving +-inf or 0).
inline size_t cjson_number(const unsigned char* p, double* d) {
    size_t n = 0;
    for (;; n++) {
        unsigned char c = p[n];
        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == 'e' || c == 'E' || c == '.')) break;
    }
#if FAST
    {
        // Up to 15 digits, with an optional '-': exact as an integer, and so as a double.
        size_t i = p[0] == '-';
        if (n - i >= 1 && n - i <= 15) {
            uint64_t v = 0;
            size_t k = i;
            while (k < n && p[k] >= '0' && p[k] <= '9') v = v * 10 + (p[k++] - '0');
            if (k == n) {
                *d = i ? -(double)v : (double)v;
                return n;
            }
        }
        const char* f = (const char*)p;
        auto r = std::from_chars(f, f + n, *d);
        if (r.ec == std::errc()) return (size_t)(r.ptr - f);
        if (r.ec == std::errc::invalid_argument) return 0;
    }
#endif
    char small[64];
    std::string big;
    char* buf = small;
    if (n >= sizeof small) { big.assign((const char*)p, n); buf = big.data(); }
    else { std::memcpy(small, p, n); small[n] = '\0'; }
    char* after;
    *d = std::strtod(buf, &after);
    return (size_t)(after - buf);
}

// --- scanning ------------------------------------------------------------------

// Skips whitespace (to cJSON, any byte <= 32). With FAST, 8 bytes at a time: a byte's
// high bit in `big` is set if the byte is 33 or more.
inline const unsigned char* json_skip_space(const unsigned char* p, const unsigned char* end) {
#if FAST
    if (p < end && *p > 32) return p;            // most values follow no whitespace
    while (p + 8 <= end) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        uint64_t big = (w | ((w & 0x7F7F7F7F7F7F7F7Full) + 0x5F5F5F5F5F5F5F5Full)) & 0x8080808080808080ull;
        if (big) return p + (std::countr_zero(big) >> 3);
        p += 8;
    }
#endif
    while (p < end && *p <= 32) p++;
    return p;
}

// The closing '"' of a string body starting at q, or null if there is none; sets
// `escaped` if the body needs decoding: it has a '\\', or a '\0' (cJSON ends a string at
// its first '\0' byte, which only cjson_decode_string does here).
inline const unsigned char* json_string_end(const unsigned char* q, const unsigned char* end, bool& escaped) {
    escaped = false;
#if FAST
    for (;;) {
        // The first '"', '\\' or '\0', 8 bytes at a time: a byte of a, b or w is 0 where it
        // matches.
        while (q + 8 <= end) {
            uint64_t w;
            std::memcpy(&w, q, 8);
            uint64_t a = w ^ 0x2222222222222222ull, b = w ^ 0x5C5C5C5C5C5C5C5Cull;
            uint64_t hit = (((a - 0x0101010101010101ull) & ~a) | ((b - 0x0101010101010101ull) & ~b) |
                            ((w - 0x0101010101010101ull) & ~w)) & 0x8080808080808080ull;
            if (hit) { q += std::countr_zero(hit) >> 3; goto found; }
            q += 8;
        }
        while (q < end && *q != '"' && *q != '\\' && *q != 0) q++;
    found:
        if (q >= end) return nullptr;
        if (*q == '"') return q;
        escaped = true;
        if (*q == 0) { q++; continue; }
        if (q + 1 >= end) return nullptr;        // a '\\' as the last byte
        q += 2;
    }
#else
    while (q < end && *q != '"') {
        if (*q == '\\') {
            if (q + 1 >= end) return nullptr;
            escaped = true;
            q++;
        } else if (*q == 0) {
            escaped = true;
        }
        q++;
    }
    return q < end ? q : nullptr;
#endif
}
