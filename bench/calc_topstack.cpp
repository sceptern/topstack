// Goose's calc benchmark (bench/goose/calc.goose) with topstack: many small parses,
// a quarter of them malformed. Each input's tree is built inside a Scope on the
// thread's plain stack and freed by the Scope's rewind when the input is done,
// whichever way it ends, with no per-node free. Nodes are only as big as their kind
// needs, linked by 2-byte Ref16s (a tree here is small, so every link is near):
// a number 5 bytes, a negation or parenthesis 3, a binary operator 6.
// Errors are returned through every level as codes, as in the expert C++ version;
// C++ has no equivalent of Goose's `return ... from`.
#include "bench.h"
#include <topstack/ref16.hpp>

#include <string>

using topstack::Packed;
using topstack::Ref16;

static const int E_EOF = 1;
static const int E_PAREN = 2;
static const int E_TOKEN = 3;
static const int E_DIV0 = 4;

static std::string text;                   // Reused across inputs: it is the input, not the subject.
static size_t pos = 0;
static long long nodes = 0;                // Every node ever built, bad inputs included.

// --- generation (as in calc.cpp) ----------------------------------------------

static void emit_num(long long v) {
    if (v >= 10) text.push_back((char)('0' + v / 10));
    text.push_back((char)('0' + v % 10));
}

static uint64_t gen(long long depth, uint64_t seed) {
    uint64_t r = xs_next(seed);
    long long k = (long long)xs_mod(r, 8);
    if (depth == 0 || k == 0) { emit_num(1 + (long long)xs_mod(r >> 8, 99)); return r; }
    if (k == 1) { text.push_back('-'); return gen(depth - 1, r); }
    if (k == 2) {
        text.push_back('(');
        r = gen(depth - 1, r);
        text.push_back(')');
        return r;
    }
    r = gen(depth - 1, r);
    long long op = (long long)xs_mod(r >> 16, 4);
    text.push_back(' ');
    text.push_back(op == 0 ? '+' : op == 1 ? '-' : op == 2 ? '*' : '/');
    text.push_back(' ');
    return gen(depth - 1, xs_next(r));
}

static void corrupt(uint64_t seed) {
    long long kind = (long long)xs_mod(seed, 16);
    if (kind == 0) {
        size_t i = text.size();
        while (i > 0) {
            i--;
            if (text[i] == ')') { text[i] = ' '; return; }
        }
        text[0] = '#';
        return;
    }
    if (kind == 1) { text[(size_t)xs_mod(seed >> 8, (uint64_t)text.size())] = '#'; return; }
    if (kind == 2) { text.resize(text.size() / 2); return; }
    if (kind == 3) { text += " / 0"; }
}

static void skip() { while (pos < text.size() && text[pos] == ' ') pos++; }

// --- the tree ----------------------------------------------------------------

struct Expr { uint8_t kind; };                                  // 0 Num, 1 Neg, 2 Paren, 3 Bin
struct Num : Expr { Packed<int32_t> v; };
struct One : Expr { Ref16<Expr> of; };                          // Neg and Paren
struct Bin : Expr { char op; Ref16<Expr> l, r; };
static_assert(sizeof(Num) == 5 && sizeof(One) == 3 && sizeof(Bin) == 6);

static int parse_expr(Expr*& out);

static int parse_factor(Expr*& out) {
    skip();
    if (pos >= text.size()) return E_EOF;
    char c = text[pos];
    if (c >= '0' && c <= '9') {
        int64_t v = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { v = v * 10 + (text[pos] - '0'); pos++; }
        nodes++;
        out = topstack::make<Num>(Expr{0}, (int32_t)v);
        return 0;
    }
    if (c == '-') {
        pos++;
        Expr* f;
        if (int e = parse_factor(f)) return e;
        nodes++;
        out = topstack::make<One>(Expr{1}, f);
        return 0;
    }
    if (c != '(') return E_TOKEN;
    pos++;
    Expr* inner;
    if (int e = parse_expr(inner)) return e;
    skip();
    if (pos >= text.size() || text[pos] != ')') return E_PAREN;
    pos++;
    nodes++;
    out = topstack::make<One>(Expr{2}, inner);
    return 0;
}

static int parse_term(Expr*& out) {
    Expr* lhs;
    if (int e = parse_factor(lhs)) return e;
    for (;;) {
        skip();
        if (pos >= text.size() || (text[pos] != '*' && text[pos] != '/')) break;
        char op = text[pos];
        pos++;
        Expr* rhs;
        if (int e = parse_factor(rhs)) return e;
        nodes++;
        lhs = topstack::make<Bin>(Expr{3}, op, lhs, rhs);
    }
    out = lhs;
    return 0;
}

static int parse_expr(Expr*& out) {
    Expr* lhs;
    if (int e = parse_term(lhs)) return e;
    for (;;) {
        skip();
        if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) break;
        char op = text[pos];
        pos++;
        Expr* rhs;
        if (int e = parse_term(rhs)) return e;
        nodes++;
        lhs = topstack::make<Bin>(Expr{3}, op, lhs, rhs);
    }
    out = lhs;
    return 0;
}

// Unsigned, so overflow wraps identically in every language.
static int eval(const Expr* e, uint64_t& out) {
    switch (e->kind) {
    case 0: out = (uint64_t)(int64_t)(int32_t)static_cast<const Num*>(e)->v; return 0;
    case 1: {
        uint64_t a;
        if (int r = eval(static_cast<const One*>(e)->of.get(), a)) return r;
        out = 0ull - a;
        return 0;
    }
    case 2: return eval(static_cast<const One*>(e)->of.get(), out);
    default: {
        const Bin* b = static_cast<const Bin*>(e);
        uint64_t x, y;
        if (int r = eval(b->l.get(), x)) return r;
        if (int r = eval(b->r.get(), y)) return r;
        if (b->op == '+') { out = x + y; return 0; }
        if (b->op == '-') { out = x - y; return 0; }
        if (b->op == '*') { out = x * y; return 0; }
        if (y == 0) return E_DIV0;
        out = x / y;
        return 0;
    }
    }
}

// One input: its tree lives in this Scope and is freed when the function returns.
static int run_one(uint64_t& out) {
    topstack::Scope scope;
    pos = 0;
    Expr* root;
    if (int e = parse_expr(root)) return e;
    skip();
    if (pos != text.size()) return E_TOKEN;
    return eval(root, out);
}

int main() {
    uint64_t seed = 12345;
    long long ok = 0, chars = 0;
    long long errs[5] = { 0, 0, 0, 0, 0 };
    uint64_t sum = 0;
    for (long long i = 0; i < BENCH_N; i++) {
        seed = xs_next(seed);
        text.clear();
        long long depth = 3 + (long long)xs_mod(seed, 4);
        uint64_t s = gen(depth, seed >> 8);
        corrupt(s);
        chars += (long long)text.size();
        uint64_t v = 0;
        int err = run_one(v);
        if (err == 0) { ok++; sum = sum * 31 + v; } else { errs[err] += 1; }
    }
    emit(chars);
    emit(nodes);
    emit(ok);
    emit(errs[E_EOF]);
    emit(errs[E_PAREN]);
    emit(errs[E_TOKEN]);
    emit(errs[E_DIV0]);
    emit_u(sum);
    return 0;
}
