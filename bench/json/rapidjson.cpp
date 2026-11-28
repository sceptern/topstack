// RapidJSON, for reference: a fast C++ JSON library whose DOM keeps every value in
// 16 bytes (short strings inline) and allocates from a memory pool.
//   INSITU=0  Document::Parse: strings are copied into the pool
//   INSITU=1  Document::ParseInsitu: strings are decoded in place, in a copy of the
//             input made each round (RapidJSON's fastest way to build a DOM)
//   FLAGS     kParseFullPrecisionFlag by default, so every number is the nearest
//             double, as with strtod (cJSON). -DFLAGS=0 is RapidJSON's faster default,
//             which reads some numbers 1 bit off (on canada.json, say).
// The walk's checksums must match cJSON's (all but the number bits for -DFLAGS=0).
// RapidJSON prints numbers its own way, so its printed text differs from cJSON's; it is
// timed, not compared.
#include "common.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <vector>

#ifndef FLAGS
#define FLAGS rapidjson::kParseFullPrecisionFlag
#endif
#ifndef INSITU
#define INSITU 0
#endif

static void walk(const rapidjson::Value& v, Walk& w) {
    w.values++;
    switch (v.GetType()) {
        case rapidjson::kNumberType: w.number(v.GetDouble()); break;
        case rapidjson::kStringType: w.string((const unsigned char*)v.GetString(), v.GetStringLength()); break;
        case rapidjson::kArrayType:
            for (const auto& e : v.GetArray()) walk(e, w);
            break;
        case rapidjson::kObjectType:
            for (const auto& m : v.GetObject()) {
                w.string((const unsigned char*)m.name.GetString(), m.name.GetStringLength());
                walk(m.value, w);
            }
            break;
        default: break;
    }
}

int main() {
    std::string text = read_input();
    long n = rounds();
    std::vector<char> copy(text.size() + 1);
    Walk w;
    size_t printed = 0;
    uint64_t hash = 0;
    for (long r = 0; r < n; r++) {
        rapidjson::Document doc;
#if INSITU
        std::memcpy(copy.data(), text.c_str(), text.size() + 1);
        doc.ParseInsitu<FLAGS>(copy.data());
#else
        doc.Parse<FLAGS>(text.c_str(), text.size());
#endif
        if (doc.HasParseError()) { std::fprintf(stderr, "parse error\n"); return 1; }
        w = Walk();
        walk(doc, w);
#if PRINT
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        doc.Accept(writer);
        printed = sb.GetSize();
        hash = hash_text(sb.GetString(), printed);
#endif
    }
    report(w, printed, hash);
}
