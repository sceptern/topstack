// Goose's respond benchmark (bench/goose/respond.goose) with topstack: per request,
// build the typed response object (an id, a user name, a list of line items), render
// it as JSON, checksum it, and throw both away. As in Goose, the response is one
// block: the name and every sku are stored inline, and the items are one array, all
// made on the thread's plain stack inside a Scope. The rendered JSON goes into a
// GrowBuf on the same stack, right after the response: nothing else is allocated there
// while it grows, so it stays on top, as a Goose local does. (A GrowBuf on a stack of
// its own would borrow one from the thread's set on every request, which costs about
// 5% here.) Both are released when the request ends, with no per-string or per-list
// free.
#include "bench.h"
#include <topstack/grow_buf.hpp>

#include <charconv>
#include <cstdlib>
#include <cstring>

using topstack::GrowBuf;

struct Item { char sku[6]; int32_t qty, price; };
struct Resp { int64_t id; uint8_t user_len; char user[10]; uint8_t nitems; Item* items; };

// A user name of 3..10 letters; one in eight carries a quote or a backslash.
static void name_of(uint64_t seed, Resp& resp) {
    uint64_t r = xs_next(seed);
    long long n = 3 + (long long)xs_mod(r, 8);
    bool odd = xs_mod(r >> 8, 8) == 0;
    for (long long i = 0; i < n; i++) {
        r = xs_next(r);
        if (odd && i == 1) resp.user[i] = xs_mod(r, 2) == 0 ? '"' : '\\';
        else resp.user[i] = (char)('a' + xs_mod(r, 26));
    }
    resp.user_len = (uint8_t)n;
}

// A stock keeping unit: two letters and four digits.
static void sku_of(uint64_t seed, char* sku) {
    uint64_t r = xs_next(seed);
    sku[0] = (char)('A' + xs_mod(r, 26));
    sku[1] = (char)('A' + xs_mod(r >> 8, 26));
    for (int i = 0; i < 4; i++) { r = xs_next(r); sku[2 + i] = (char)('0' + xs_mod(r, 10)); }
}

static Item* make_items(uint64_t seed, long long n) {
    Item* t = topstack::make_array<Item>((size_t)n);
    uint64_t r = seed;
    for (long long i = 0; i < n; i++) {
        r = xs_next(r);
        sku_of(r, t[i].sku);
        t[i].qty = (int32_t)(1 + xs_mod(r >> 8, 5));
        t[i].price = (int32_t)(100 + xs_mod(r >> 16, 99900));
    }
    return t;
}

// --- rendering ---------------------------------------------------------------

static void put(GrowBuf<char>& out, const char* s, size_t n) {
    char* p = out.extend(n);
    if (!p) std::abort();
    std::memcpy(p, s, n);
}
template <size_t N> static void lit(GrowBuf<char>& out, const char (&s)[N]) { put(out, s, N - 1); }
static void ch(GrowBuf<char>& out, char c) { if (!out.push(c)) std::abort(); }

static void emit_int(GrowBuf<char>& out, long long v) {
    char buf[24];
    std::to_chars_result res = std::to_chars(buf, buf + sizeof(buf), v);
    put(out, buf, (size_t)(res.ptr - buf));
}

static void emit_str(GrowBuf<char>& out, const char* s, size_t n) {
    ch(out, '"');
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\') ch(out, '\\');
        ch(out, s[i]);
    }
    ch(out, '"');
}

static void render(GrowBuf<char>& out, const Resp& resp) {
    lit(out, "{\"id\":");
    emit_int(out, resp.id);
    lit(out, ",\"user\":");
    emit_str(out, resp.user, resp.user_len);
    lit(out, ",\"items\":[");
    long long total = 0;
    for (int i = 0; i < resp.nitems; i++) {
        const Item& it = resp.items[i];
        if (i > 0) ch(out, ',');
        lit(out, "{\"sku\":");
        emit_str(out, it.sku, 6);
        lit(out, ",\"qty\":");
        emit_int(out, it.qty);
        lit(out, ",\"price\":");
        emit_int(out, it.price);
        ch(out, '}');
        total += (long long)it.qty * it.price;
    }
    lit(out, "],\"total\":");
    emit_int(out, total);
    ch(out, '}');
}

static uint64_t fnv(const char* s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ull;
    return h;
}

// One request: the response object and its rendering are both locals.
static uint64_t handle(uint64_t seed, long long& len) {
    topstack::Scope scope;
    uint64_t r = xs_next(seed);
    long long n = 1 + (long long)xs_mod(r, 8);
    Resp* resp = topstack::make<Resp>();
    resp->id = (int64_t)xs_mod(r >> 4, 1000000);
    name_of(r >> 8, *resp);
    resp->nitems = (uint8_t)n;
    resp->items = make_items(r >> 16, n);
    GrowBuf<char> out(topstack::plain_stack());   // right on top of the response, like a Goose local
    render(out, *resp);
    len = (long long)out.size();
    return fnv(out.data(), out.size());
}

int main() {
    uint64_t seed = 12345, h = 0;
    long long bytes = 0;
    for (long long i = 0; i < BENCH_N; i++) {
        seed = xs_next(seed);
        long long l = 0;
        uint64_t hh = handle(seed, l);
        h = h ^ (hh * 31 + (uint64_t)i);
        bytes += l;
    }
    emit(BENCH_N);
    emit(bytes);
    emit_u(h);
    return 0;
}
