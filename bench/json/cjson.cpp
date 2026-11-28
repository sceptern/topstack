// cJSON itself (cJSON.c 1.7.19, unchanged; compare_json.sh downloads it to json/data/cjson/),
// two ways:
//   HOOKS=0  as shipped: malloc and free
//   HOOKS=1  topstack level 1: cJSON_InitHooks hands cJSON a topstack stack. Each
//            allocation is a bump of the stack's top; free only takes back the newest
//            allocation (cJSON frees its number buffers right after making them),
//            and a Scope frees the whole document at once, without cJSON_Delete.
// See common.h for what each round does.
#ifndef HOOKS
#define HOOKS 0
#endif
#include "common.h"
extern "C" {
#include "cjson/cJSON.h"
}
#if HOOKS
#include <topstack/stack.hpp>
#endif

#if HOOKS
static topstack::Stack* doc_stack;
static void* newest;                       // the newest allocation, and the top before it
static topstack::Stack::Mark before_newest;

static void* ts_malloc(size_t n) {
    // What malloc promises: aligned for any object of this size (cJSON's 64-byte nodes
    // get 16, its strings 1).
    size_t align = n & (~n + 1);
    if (align > 16 || align == 0) align = 16;
    before_newest = doc_stack->mark();
    newest = doc_stack->alloc(n, align);
    return newest;
}
static void ts_free(void* p) {
    if (p && p == newest) {
        doc_stack->rewind(before_newest);
        newest = nullptr;
    }
}
#endif

static void walk(const cJSON* n, Walk& w) {
    for (; n; n = n->next) {
        w.values++;
        if (n->string) w.string((const unsigned char*)n->string, std::strlen(n->string));
        switch (n->type & 0xFF) {
            case cJSON_Number: w.number(n->valuedouble); break;
            case cJSON_String: w.string((const unsigned char*)n->valuestring, std::strlen(n->valuestring)); break;
            case cJSON_Array:
            case cJSON_Object: walk(n->child, w); break;
        }
    }
}

int main() {
    std::string text = read_input();
    long n = rounds();
#if HOOKS
    auto created = topstack::Stack::create();
    if (!created.stack) { std::fprintf(stderr, "%s\n", topstack::to_string(created.error)); return 1; }
    doc_stack = created.stack.get();
    cJSON_Hooks hooks = {ts_malloc, ts_free};
    cJSON_InitHooks(&hooks);
#endif
    Walk w;
    size_t printed = 0;
    uint64_t hash = 0;
    for (long r = 0; r < n; r++) {
#if HOOKS
        topstack::Scope scope(*doc_stack);
        newest = nullptr;
#endif
        cJSON* doc = cJSON_ParseWithLength(text.data(), text.size() + 1);
        if (!doc) { std::fprintf(stderr, "parse error\n"); return 1; }
        w = Walk();
        walk(doc, w);
#if PRINT
        char* out = cJSON_PrintUnformatted(doc);
        printed = std::strlen(out);
        hash = hash_text(out, printed);
#endif
#if !HOOKS
#if PRINT
        cJSON_free(out);
#endif
        cJSON_Delete(doc);
#endif
    }
    report(w, printed, hash);
}
