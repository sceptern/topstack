// topstack level 3: the level-2 port (topstack.cpp) with 1-byte links. Same
// parser and printer, same output byte for byte; only the layout differs.
//
// A Link8 is set once, when its node is made, so the tree is built bottom-up: a value's
// bytes first, then its record, which links back to things already made:
//
//   [key bytes][string bytes][record]            a number, string, true, false or null
//   [children...][key bytes][record]             an array or object
//
//   record: [tag 1][prev: OptLink8 1][last: OptLink8 1, arrays and objects]
//           [key length, members][string length, strings][double 8, numbers]
//
// prev is the previous sibling, last the last child: both were made before the record.
// A Link8 holds a target within 64 bytes directly; farther ones (a sibling before a big
// array, say) get a 4-byte anchor right after the record. A number in an array takes 10
// bytes, a member with a short string key and value 4 bytes plus its text.
//
// Built with -DFAST=1 (cjson_link8_fast), numbers and scanning use faster code with the
// same results; see common.h.
//
// Parse-only: the links can't be changed once made, so the tree can be read and printed
// but not edited. Printing goes through each container's children last to first, so it
// gathers them first (in a reused vector) to print them in order.
#include "common.h"
#include <topstack/grow_buf.hpp>
#include <topstack/link8.hpp>
#include <new>
#include <vector>

namespace ts = topstack;

enum : uint8_t { T_NULL, T_FALSE, T_TRUE, T_NUMBER, T_STRING, T_ARRAY, T_OBJECT, HAS_KEY = 0x80 };

struct Node { uint8_t tag; };
using Link = ts::OptLink8<Node>;

static bool is_list(uint8_t tag) { uint8_t t = tag & 0x7F; return t == T_ARRAY || t == T_OBJECT; }

static size_t length_size(size_t n) { return n < 255 ? 1 : 5; }
static unsigned char* put_length(unsigned char* p, size_t n) {
    if (n < 255) { *p = (unsigned char)n; return p + 1; }
    p[0] = 0xFF;
    uint32_t v = (uint32_t)n;
    std::memcpy(p + 1, &v, 4);
    return p + 5;
}
static size_t get_length(const unsigned char*& p) {
    if (*p != 0xFF) return *p++;
    uint32_t v;
    std::memcpy(&v, p + 1, 4);
    p += 5;
    return v;
}

// --- parsing -----------------------------------------------------------------

struct Parser {
    ts::Stack& s;
    const unsigned char* p;
    const unsigned char* end;
    int depth = 0;
    std::string scratch;

    void skip_space() { p = json_skip_space(p, end); }

    // Finds the closing quote of the string at p. Returns null if there is none.
    const unsigned char* string_end(bool& escaped) { return json_string_end(p + 1, end, escaped); }

    // Decodes the string body [p + 1, q) to the stack; returns its length, or -1.
    long string_to_stack(const unsigned char* q, bool escaped) {
        const unsigned char* body = p + 1;
        if (!escaped) {
            size_t n = (size_t)(q - body);
            std::memcpy(s.alloc(n, 1), body, n);
            return (long)n;
        }
        scratch.resize((size_t)(q - body));
        long n = cjson_decode_string(body, q, reinterpret_cast<unsigned char*>(scratch.data()));
        if (n >= 0) std::memcpy(s.alloc((size_t)n, 1), scratch.data(), (size_t)n);
        return n;
    }

    // Makes a record. It must be the newest allocation when its links are set, so a far
    // link's anchor can go right after it.
    Node* record(uint8_t tag, Node* prev, Node* last, size_t key_len, size_t str_len, double num) {
        uint8_t t = tag & 0x7F;
        size_t size = 2 + (is_list(tag) ? 1 : 0) + ((tag & HAS_KEY) ? length_size(key_len) : 0) +
                      (t == T_STRING ? length_size(str_len) : 0) + (t == T_NUMBER ? 8 : 0);
        unsigned char* r = static_cast<unsigned char*>(s.alloc(size, 1));
        r[0] = tag;
        unsigned char* q = r + 2 + (is_list(tag) ? 1 : 0);
        if (tag & HAS_KEY) q = put_length(q, key_len);
        if (t == T_STRING) q = put_length(q, str_len);
        if (t == T_NUMBER) std::memcpy(q, &num, 8);
        new (r + 1) Link(prev);
        if (is_list(tag)) new (r + 2) Link(last);
        return reinterpret_cast<Node*>(r);
    }

    // The value at p. A member's key is either already on the stack (key_len bytes,
    // for values without children) or in `pending`, to go after the children.
    Node* value(Node* prev, uint8_t key, size_t key_len, const std::string* pending) {
        unsigned char c = *p;
        if (c == 'n' && std::strncmp((const char*)p, "null", 4) == 0) { p += 4; return record(key | T_NULL, prev, nullptr, key_len, 0, 0); }
        if (c == 'f' && std::strncmp((const char*)p, "false", 5) == 0) { p += 5; return record(key | T_FALSE, prev, nullptr, key_len, 0, 0); }
        if (c == 't' && std::strncmp((const char*)p, "true", 4) == 0) { p += 4; return record(key | T_TRUE, prev, nullptr, key_len, 0, 0); }
        if (c == '"') {
            bool escaped;
            const unsigned char* q = string_end(escaped);
            if (!q) return nullptr;
            long n = string_to_stack(q, escaped);
            if (n < 0) return nullptr;
            p = q + 1;
            return record(key | T_STRING, prev, nullptr, key_len, (size_t)n, 0);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            double d;
            size_t used = cjson_number(p, &d);
            if (used == 0) return nullptr;
            p += used;
            return record(key | T_NUMBER, prev, nullptr, key_len, 0, d);
        }
        if (c == '[' || c == '{') {
            bool object = c == '{';
            if (depth >= 1000) return nullptr;
            depth++;
            Node* last = nullptr;
            p++;
            skip_space();
            if (*p == (object ? '}' : ']')) {
                p++;
            } else {
                for (;;) {
                    skip_space();
                    last = object ? member(last) : value(last, 0, 0, nullptr);
                    if (!last) return nullptr;
                    skip_space();
                    if (*p == ',') { p++; continue; }
                    if (*p == (object ? '}' : ']')) { p++; break; }
                    return nullptr;
                }
            }
            depth--;
            if (pending) std::memcpy(s.alloc(key_len, 1), pending->data(), key_len);
            return record(key | (object ? T_OBJECT : T_ARRAY), prev, last, key_len, 0, 0);
        }
        return nullptr;
    }

    // "key": value. The key goes on the stack now if the value has no children, and
    // after them if it has.
    Node* member(Node* prev) {
        if (*p != '"') return nullptr;
        bool escaped;
        const unsigned char* q = string_end(escaped);
        if (!q) return nullptr;
        const unsigned char* key_at = p;
        p = q + 1;
        skip_space();
        if (*p != ':') return nullptr;
        p++;
        skip_space();
        if (*p == '[' || *p == '{') {
            std::string key;
            const unsigned char* after = p;
            p = key_at;
            key.resize((size_t)(q - (p + 1)));
            long n = escaped ? cjson_decode_string(p + 1, q, reinterpret_cast<unsigned char*>(key.data()))
                             : (std::memcpy(key.data(), p + 1, key.size()), (long)key.size());
            if (n < 0) return nullptr;
            p = after;
            return value(prev, HAS_KEY, (size_t)n, &key);
        }
        const unsigned char* after = p;
        p = key_at;
        long n = string_to_stack(q, escaped);
        if (n < 0) return nullptr;
        p = after;
        return value(prev, HAS_KEY, (size_t)n, nullptr);
    }
};

static const Node* parse(ts::Stack& s, const std::string& text) {
    Parser ps{s, (const unsigned char*)text.data(), (const unsigned char*)text.data() + text.size(), 0, {}};
    if (text.size() >= 4 && std::memcmp(ps.p, "\xEF\xBB\xBF", 3) == 0) ps.p += 3;
    ps.skip_space();
    return ps.value(nullptr, 0, 0, nullptr);
}

// --- reading -----------------------------------------------------------------

struct View {
    uint8_t type;
    const Node* last = nullptr;
    const unsigned char* key = nullptr;
    size_t key_len = 0;
    const unsigned char* str = nullptr;
    size_t str_len = 0;
    double num = 0;
};
static const Node* prev_of(const Node* n) {
    return reinterpret_cast<const Link*>(reinterpret_cast<const unsigned char*>(n) + 1)->get();
}
static View view(const Node* n) {
    View v;
    const unsigned char* r = reinterpret_cast<const unsigned char*>(n);
    uint8_t tag = r[0];
    v.type = tag & 0x7F;
    const unsigned char* q = r + 2;
    if (is_list(tag)) {
        v.last = reinterpret_cast<const Link*>(r + 2)->get();
        q++;
    }
    if (tag & HAS_KEY) v.key_len = get_length(q);
    if (v.type == T_STRING) v.str_len = get_length(q);
    if (v.type == T_NUMBER) std::memcpy(&v.num, q, 8);
    v.str = r - v.str_len;
    v.key = (tag & HAS_KEY) ? v.str - v.key_len : nullptr;
    return v;
}

static void walk(const Node* n, Walk& w) {
    w.values++;
    View v = view(n);
    if (v.key) w.string(v.key, v.key_len);
    switch (v.type) {
        case T_NUMBER: w.number(v.num); break;
        case T_STRING: w.string(v.str, v.str_len); break;
        case T_ARRAY:
        case T_OBJECT:
            for (const Node* c = v.last; c; c = prev_of(c)) walk(c, w);
            break;
    }
}

// --- printing ------------------------------------------------------------------

struct Out {
    ts::GrowBuf<char> buf;
    char* extend(size_t n) {
        char* p = buf.extend(n);
        if (!p) { std::fprintf(stderr, "out of memory\n"); std::exit(1); }
        return p;
    }
    void put(char c) { *extend(1) = c; }
};

static std::vector<const Node*> order;      // children being printed, last first

[[maybe_unused]] static void print(const Node* n, Out& out) {
    View v = view(n);
    if (v.key) {
        cjson_put_string(out, v.key, v.key_len);
        out.put(':');
    }
    switch (v.type) {
        case T_NULL: std::memcpy(out.extend(4), "null", 4); break;
        case T_FALSE: std::memcpy(out.extend(5), "false", 5); break;
        case T_TRUE: std::memcpy(out.extend(4), "true", 4); break;
        case T_NUMBER: cjson_put_number(out, v.num); break;
        case T_STRING: cjson_put_string(out, v.str, v.str_len); break;
        case T_ARRAY:
        case T_OBJECT: {
            bool object = v.type == T_OBJECT;
            out.put(object ? '{' : '[');
            size_t base = order.size();
            for (const Node* c = v.last; c; c = prev_of(c)) order.push_back(c);
            for (size_t i = order.size(); i-- > base;) {
                print(order[i], out);
                if (i > base) out.put(',');
            }
            order.resize(base);
            out.put(object ? '}' : ']');
            break;
        }
    }
}

int main() {
    std::string text = read_input();
    long n = rounds();
    ts::Stack& s = ts::plain_stack();
    Walk w;
    size_t printed = 0;
    uint64_t hash = 0;
    for (long r = 0; r < n; r++) {
        ts::Scope scope(s);
        const Node* doc = parse(s, text);
        if (!doc) { std::fprintf(stderr, "parse error\n"); return 1; }
        w = Walk();
        walk(doc, w);
#if PRINT
        Out out;
        print(doc, out);
        printed = out.buf.size();
        hash = hash_text(out.buf.data(), printed);
#endif
    }
    report(w, printed, hash);
}
