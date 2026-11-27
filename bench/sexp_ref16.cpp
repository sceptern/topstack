// Goose's sexp benchmark (bench/goose/sexp.goose) with Ref16<T> links, otherwise the
// same as sexp_topstack.cpp: nodes only as big as their kind needs, symbol text
// inline after its node, first-child / previous-sibling links, and the list of
// roots in a GrowBuf on another stack. Nodes: symbol 4 bytes + text, number 7,
// list 5 (with Ref<T>: 6, 9 and 9).
#include "bench.h"
#include <topstack/grow_buf.hpp>
#include <topstack/ref16.hpp>
#include <cstring>

#ifndef DEPTH
#define DEPTH 4
#endif
static const int PASSES = 4;

struct Node { uint8_t kind; };                                    // 0 sym, 1 num, 2 list
struct Sym  : Node { uint8_t len; topstack::Ref16<Node> next; };    // then `len` bytes of text
struct Num  : Node { topstack::Packed<int32_t> v; topstack::Ref16<Node> next; };
struct List : Node { topstack::Ref16<Node> first, next; };
static_assert(sizeof(Sym) == 4 && sizeof(Num) == 7 && sizeof(List) == 5);
static const char* name_of(const Sym* s) { return reinterpret_cast<const char*>(s + 1); }

static topstack::GrowBuf<char> text;
static size_t pos = 0;
static long long nodes = 0;

// --- generation --------------------------------------------------------------

static void emit_sym(uint64_t seed) {
    uint64_t r = seed;
    long long n = 3 + (long long)xs_mod(r, 12);
    for (long long i = 0; i < n; i++) { r = xs_next(r); (void)text.push((char)('a' + xs_mod(r, 26))); }
}
static void emit_num(long long v) {
    if (v == 0) { (void)text.push('0'); return; }
    char tmp[24];
    int k = 0;
    while (v > 0) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    while (k > 0) (void)text.push(tmp[--k]);
}
static uint64_t gen(int depth, uint64_t seed) {
    uint64_t r = xs_next(seed);
    if (depth == 0 || xs_mod(r, 4) == 0) {
        if (xs_mod(r >> 8, 2) == 0) emit_sym(r);
        else emit_num((long long)xs_mod(r >> 16, 100000));
        (void)text.push(' ');
        return r;
    }
    (void)text.push('(');
    long long kids = 2 + (long long)xs_mod(r >> 24, 4);
    for (long long k = 0; k < kids; k++) r = gen(depth - 1, r);
    (void)text.push(')');
    (void)text.push(' ');
    return r;
}

// --- parsing -----------------------------------------------------------------

static void skip_space() { while (pos < text.size() && text[pos] == ' ') pos++; }

static Node* parse_node(topstack::Stack& s, Node* prev) {
    skip_space();
    nodes++;
    char c = text[pos];
    if (c == '(') {
        pos++;
        Node* chain = nullptr;
        for (;;) {
            skip_space();
            if (text[pos] == ')') { pos++; break; }
            chain = parse_node(s, chain);
        }
        return s.make<List>(Node{2}, chain, prev);
    }
    size_t start = pos;
    if (c >= '0' && c <= '9') {
        long long v = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { v = v * 10 + (text[pos] - '0'); pos++; }
        return s.make<Num>(Node{1}, (int32_t)v, prev);
    }
    while (pos < text.size() && text[pos] != ' ' && text[pos] != '(' && text[pos] != ')') pos++;
    size_t len = pos - start;
    void* mem = s.alloc(sizeof(Sym) + len, alignof(Sym));
    Sym* n = new (mem) Sym{Node{0}, (uint8_t)len, prev};
    std::memcpy(const_cast<char*>(name_of(n)), text.data() + start, len);
    return n;
}

// --- walking -----------------------------------------------------------------

static topstack::Ref16<Node> const& next_of(const Node* n) {
    switch (n->kind) {
        case 0: return static_cast<const Sym*>(n)->next;
        case 1: return static_cast<const Num*>(n)->next;
        default: return static_cast<const List*>(n)->next;
    }
}
static long long walk(const Node* n, long long p);
static long long walk_chain(const Node* c, long long p) { return c ? walk(c, p) + walk_chain(next_of(c).get(), p) : 0; }
static long long walk(const Node* n, long long p) {
    switch (n->kind) {
        case 0: { auto* y = static_cast<const Sym*>(n); return (long long)y->len + (unsigned char)name_of(y)[0] + p; }
        case 1: return (int32_t)static_cast<const Num*>(n)->v ^ p;
        default: return 1 + walk_chain(static_cast<const List*>(n)->first.get(), p);
    }
}

int main() {
    uint64_t seed = 12345;
    for (long long i = 0; i < BENCH_N; i++) seed = gen(DEPTH, seed);
    topstack::Stack& s = topstack::plain_stack();
    topstack::GrowBuf<Node*> roots;
    for (;;) {
        skip_space();
        if (pos >= text.size()) break;
        (void)roots.push(parse_node(s, nullptr));
    }
    long long total = 0;
    for (long long p = 0; p < PASSES; p++)
        for (Node* r : roots) total += walk(r, p);
    emit((long long)text.size());
    emit(nodes);
    emit((long long)roots.size());
    emit(0);
    emit(total);
    return 0;
}
