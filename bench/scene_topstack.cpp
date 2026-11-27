// Goose's scene benchmark (bench/goose/scene.goose) with topstack: a scene graph whose
// nodes sit in one pool that grows in place, each with up to four children linked by
// 4-byte self-relative links inline in the node, as in Goose. Only the first nkids
// links are ever read, so they are NonNullRefs: following one is a plain add. A node is 120 bytes
// (Goose packs it to 117; the C++ alignment of its floats pads it to 120, the same as
// the expert C++ version). Nodes never move, so the recursion holds plain pointers.
#include "bench.h"
#include <topstack/grow_buf.hpp>
#include <topstack/ref.hpp>

#include <cstdlib>

using topstack::GrowBuf;
using topstack::Ref;

#ifndef DEPTH
#define DEPTH BENCH_N
#endif
static const long long FRAMES = 16;
static const int MAXKIDS = 4;

struct F3 { float x, y, z; };
struct Xf { float m[9]; F3 t; };

static const Xf IDENT = { { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };

struct Node {
    int32_t id;
    Xf local, world;
    Ref<Node> kids[MAXKIDS];
    uint8_t nkids;
};

static GrowBuf<Node>* pool;

// One of twelve axis-aligned rotations, from the seed.
static void rotation(uint64_t seed, float* m) {
    long long axis = (long long)xs_mod(seed, 3);
    long long quarter = (long long)xs_mod(seed >> 8, 4);
    float c = 1.0f, s = 0.0f;
    if (quarter == 1) { c = 0.0f; s = 1.0f; }
    else if (quarter == 2) { c = -1.0f; s = 0.0f; }
    else if (quarter == 3) { c = 0.0f; s = -1.0f; }
    if (axis == 0) {
        m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f;
        m[3] = 0.0f; m[4] = c;    m[5] = -s;
        m[6] = 0.0f; m[7] = s;    m[8] = c;
    } else if (axis == 1) {
        m[0] = c;    m[1] = 0.0f; m[2] = s;
        m[3] = 0.0f; m[4] = 1.0f; m[5] = 0.0f;
        m[6] = -s;   m[7] = 0.0f; m[8] = c;
    } else {
        m[0] = c;    m[1] = -s;   m[2] = 0.0f;
        m[3] = s;    m[4] = c;    m[5] = 0.0f;
        m[6] = 0.0f; m[7] = 0.0f; m[8] = 1.0f;
    }
}

// world = parent o local: a 3x3 product and a transformed translation.
static void compose(const Xf& p, const Xf& l, Xf& w) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++) s = s + p.m[i * 3 + k] * l.m[k * 3 + j];
            w.m[i * 3 + j] = s;
        }
    }
    w.t.x = p.m[0] * l.t.x + p.m[1] * l.t.y + p.m[2] * l.t.z + p.t.x;
    w.t.y = p.m[3] * l.t.x + p.m[4] * l.t.y + p.m[5] * l.t.z + p.t.y;
    w.t.z = p.m[6] * l.t.x + p.m[7] * l.t.y + p.m[8] * l.t.z + p.t.z;
}

// The node is pushed before its children, so ids are the pre-order creation order.
static Node* build(long long depth, uint64_t seed) {
    uint64_t r = xs_next(seed);
    Node* nd = pool->push();
    if (!nd) std::abort();
    nd->id = (int32_t)(pool->size() - 1);
    rotation(r, nd->local.m);
    nd->local.t = F3 { (float)xs_mod(r >> 16, 9), (float)xs_mod(r >> 24, 9), (float)xs_mod(r >> 32, 9) };
    nd->world = IDENT;
    nd->nkids = 0;
    if (depth == 0) return nd;
    long long n = 1 + (long long)xs_mod(r >> 40, MAXKIDS);
    for (long long i = 0; i < n; i++)
        nd->kids[nd->nkids++] = build(depth - 1, xs_next(r + (uint64_t)i));
    return nd;
}

static void update(Node* n, const Xf& parent) {
    compose(parent, n->local, n->world);
    for (uint8_t k = 0; k < n->nkids; k++) update(n->kids[k].get(), n->world);
}

int main() {
    GrowBuf<Node> nodes;
    pool = &nodes;
    Node* root = build(DEPTH, 12345);
    long long total = 0;
    for (long long f = 0; f < FRAMES; f++) {
        // A seventh of the nodes move each frame: the animation.
        long long phase = f % 7;
        float step = (float)(f % 5);
        for (Node& n : nodes)
            if (n.id % 7 == phase) { n.local.t.x = step; n.local.t.z = 8.0f - step; }
        update(root, IDENT);
        // Where everything ended up, as an integer: exact in f32, summed in i64.
        for (const Node& n : nodes) total += (long long)(n.world.t.x + n.world.t.y + n.world.t.z);
    }
    emit((long long)nodes.size());
    emit(total);
    return 0;
}
