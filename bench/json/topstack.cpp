// topstack level 2: cJSON's parser and printer (same algorithm, same output, byte for
// byte), with the tree laid out the topstack way. A cJSON node is 64 bytes, and every
// key and string is a separate malloc. Here a value is a 5-byte header followed by its
// own data, all contiguous on one stack:
//
//   [tag 1][next: OptRef 4]  [key: length + bytes, members only]  [payload]
//   payload:  number  8-byte double (Packed)
//             string  length + bytes
//             array   first child: OptRef 4, then the children
//             object  first member: OptRef 4, then the members
//             null, true, false: nothing
//
// Lengths take 1 byte below 255, else 0xFF and 4 bytes. Links are self-relative 32-bit
// OptRefs and nothing is aligned, so the tree has no padding. The tree is built in text
// order, top-down like cJSON's, and a Scope frees it in one step.
//
// Built with -DFAST=1 (cjson_ts_fast), numbers and scanning use faster code with the
// same results; see common.h.
//
// This covers what the benchmark needs: parse, read (walk) and print. Editing (cJSON's
// AddItem, Replace, Delete) would need nodes that can move or grow; not ported.
#include "common.h"
#include <topstack/grow_buf.hpp>
#include <topstack/ref.hpp>
#include <new>

namespace ts = topstack;

enum : uint8_t { T_NULL, T_FALSE, T_TRUE, T_NUMBER, T_STRING, T_ARRAY, T_OBJECT, HAS_KEY = 0x80 };

struct Node {
    uint8_t tag;
    ts::OptRef<Node> next;
};
static_assert(sizeof(Node) == 5 && alignof(Node) == 1);

// --- lengths -------------------------------------------------------------------

static unsigned char* put_length(ts::Stack& s, size_t n) {
    if (n < 255) {
        unsigned char* p = static_cast<unsigned char*>(s.alloc(1, 1));
        *p = (unsigned char)n;
        return p;
    }
    unsigned char* p = static_cast<unsigned char*>(s.alloc(5, 1));
    p[0] = 0xFF;
    uint32_t v = (uint32_t)n;
    std::memcpy(p + 1, &v, 4);
    return p;
}

static size_t get_length(const unsigned char*& p) {
    if (*p != 0xFF) return *p++;
    uint32_t v;
    std::memcpy(&v, p + 1, 4);
    p += 5;
    return v;
}

// --- parsing (cJSON's parse_value, parse_string, parse_array, parse_object) --------

struct Parser {
    ts::Stack& s;
    const unsigned char* p;
    const unsigned char* end;            // the text's terminating '\0'
    int depth = 0;
    std::string scratch;                 // for decoding strings with escapes

    void skip_space() { p = json_skip_space(p, end); }

    // A string at p, as length + bytes on the stack.
    bool string() {
        if (*p != '"') return false;
        bool escaped;
        const unsigned char* q = json_string_end(p + 1, end, escaped);
        if (!q) return false;
        const unsigned char* body = p + 1;
        if (!escaped) {
            size_t n = (size_t)(q - body);
            put_length(s, n);
            std::memcpy(s.alloc(n, 1), body, n);
        } else {
            scratch.resize((size_t)(q - body));
            long n = cjson_decode_string(body, q, reinterpret_cast<unsigned char*>(scratch.data()));
            if (n < 0) return false;
            put_length(s, (size_t)n);
            std::memcpy(s.alloc((size_t)n, 1), scratch.data(), (size_t)n);
        }
        p = q + 1;
        return true;
    }

    // The value at p, into n (whose header, and key if any, are already on the stack).
    bool value(Node* n) {
        uint8_t key = n->tag & HAS_KEY;
        unsigned char c = *p;
        if (c == 'n' && std::strncmp((const char*)p, "null", 4) == 0) { n->tag = key | T_NULL; p += 4; return true; }
        if (c == 'f' && std::strncmp((const char*)p, "false", 5) == 0) { n->tag = key | T_FALSE; p += 5; return true; }
        if (c == 't' && std::strncmp((const char*)p, "true", 4) == 0) { n->tag = key | T_TRUE; p += 4; return true; }
        if (c == '"') { n->tag = key | T_STRING; return string(); }
        if (c == '-' || (c >= '0' && c <= '9')) {
            double d;
            size_t used = cjson_number(p, &d);
            if (used == 0) return false;
            p += used;
            n->tag = key | T_NUMBER;
            new (s.alloc(8, 1)) ts::Packed<double>(d);
            return true;
        }
        if (c == '[') { n->tag = key | T_ARRAY; return list(false); }
        if (c == '{') { n->tag = key | T_OBJECT; return list(true); }
        return false;
    }

    // An array or object at p: the first-child link, then the children.
    bool list(bool object) {
        if (depth >= 1000) return false;           // cJSON's CJSON_NESTING_LIMIT
        depth++;
        auto* first = new (s.alloc(sizeof(ts::OptRef<Node>), 1)) ts::OptRef<Node>();
        p++;
        skip_space();
        if (*p == (object ? '}' : ']')) { p++; depth--; return true; }
        Node* prev = nullptr;
        for (;;) {
            Node* e = new (s.alloc(sizeof(Node), 1)) Node{object ? (uint8_t)HAS_KEY : (uint8_t)0, nullptr};
            if (prev) prev->next = e;
            else *first = e;
            prev = e;
            skip_space();
            if (object) {
                if (!string()) return false;
                skip_space();
                if (*p != ':') return false;
                p++;
                skip_space();
            }
            if (!value(e)) return false;
            skip_space();
            if (*p == ',') { p++; continue; }
            if (*p == (object ? '}' : ']')) { p++; break; }
            return false;
        }
        depth--;
        return true;
    }
};

static const Node* parse(ts::Stack& s, const std::string& text) {
    Parser ps{s, (const unsigned char*)text.data(), (const unsigned char*)text.data() + text.size(), 0, {}};
    if (text.size() >= 4 && std::memcmp(ps.p, "\xEF\xBB\xBF", 3) == 0) ps.p += 3;
    Node* root = new (s.alloc(sizeof(Node), 1)) Node{0, nullptr};
    ps.skip_space();
    return ps.value(root) ? root : nullptr;
}

// --- reading -----------------------------------------------------------------

// The node's key (if any) and where its payload starts.
struct Parts {
    const unsigned char* key = nullptr;
    size_t key_len = 0;
    const unsigned char* payload;
};
static Parts parts(const Node* n) {
    Parts r;
    const unsigned char* q = reinterpret_cast<const unsigned char*>(n) + sizeof(Node);
    if (n->tag & HAS_KEY) {
        r.key_len = get_length(q);
        r.key = q;
        q += r.key_len;
    }
    r.payload = q;
    return r;
}
static const Node* first_child(const unsigned char* payload) {
    return reinterpret_cast<const ts::OptRef<Node>*>(payload)->get();
}

static void walk(const Node* n, Walk& w) {
    for (; n; n = n->next.get()) {
        w.values++;
        Parts pt = parts(n);
        if (pt.key) w.string(pt.key, pt.key_len);
        switch (n->tag & 0x7F) {
            case T_NUMBER: w.number(ts::Packed<double>(*reinterpret_cast<const ts::Packed<double>*>(pt.payload))); break;
            case T_STRING: {
                const unsigned char* q = pt.payload;
                size_t len = get_length(q);
                w.string(q, len);
                break;
            }
            case T_ARRAY:
            case T_OBJECT: walk(first_child(pt.payload), w); break;
        }
    }
}

// --- printing (cJSON_PrintUnformatted) -------------------------------------------

struct Out {
    ts::GrowBuf<char> buf;
    char* extend(size_t n) {
        char* p = buf.extend(n);
        if (!p) { std::fprintf(stderr, "out of memory\n"); std::exit(1); }
        return p;
    }
    void put(char c) { *extend(1) = c; }
};

static void print(const Node* n, Out& out);

static void print_value(const Node* n, const unsigned char* payload, Out& out) {
    switch (n->tag & 0x7F) {
        case T_NULL: std::memcpy(out.extend(4), "null", 4); break;
        case T_FALSE: std::memcpy(out.extend(5), "false", 5); break;
        case T_TRUE: std::memcpy(out.extend(4), "true", 4); break;
        case T_NUMBER: cjson_put_number(out, ts::Packed<double>(*reinterpret_cast<const ts::Packed<double>*>(payload))); break;
        case T_STRING: {
            const unsigned char* q = payload;
            size_t len = get_length(q);
            cjson_put_string(out, q, len);
            break;
        }
        case T_ARRAY:
        case T_OBJECT: {
            bool object = (n->tag & 0x7F) == T_OBJECT;
            out.put(object ? '{' : '[');
            for (const Node* c = first_child(payload); c; c = c->next.get()) {
                print(c, out);
                if (c->next) out.put(',');
            }
            out.put(object ? '}' : ']');
            break;
        }
    }
}

[[maybe_unused]] static void print(const Node* n, Out& out) {
    Parts pt = parts(n);
    if (pt.key) {
        cjson_put_string(out, pt.key, pt.key_len);
        out.put(':');
    }
    print_value(n, pt.payload, out);
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
