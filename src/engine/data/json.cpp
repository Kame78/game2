#include "engine/data/json.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine::data {

namespace {

    const Json kNull{};

    struct Parser {
        const char* p = nullptr;
        bool ok = true;

        void skip() {
            while (*p) {
                if (*p == '/' && *(p + 1) == '/') {
                    while (*p && *p != '\n') ++p;
                    continue;
                }
                if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') { ++p; continue; }
                break;
            }
        }

        bool match(char c) {
            skip();
            if (*p != c) return false;
            ++p;
            return true;
        }

        std::string parseString() {
            std::string out;
            skip();
            if (*p != '"') { ok = false; return out; }
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1)) {
                    ++p;
                    switch (*p) {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case 'r': out.push_back('\r'); break;
                        default:  out.push_back(*p);   break;
                    }
                    ++p;
                    continue;
                }
                out.push_back(*p++);
            }
            if (*p == '"') ++p;
            else ok = false;
            return out;
        }

        Json parseValue() {
            Json v;
            skip();
            if (!*p) { ok = false; return v; }

            if (*p == '{') {
                ++p;
                v.type = Json::Type::Object;
                skip();
                while (ok && *p && *p != '}') {
                    std::string key = parseString();
                    if (!ok) break;
                    if (!match(':')) { ok = false; break; }
                    v.pairs.emplace_back(std::move(key), parseValue());
                    skip();
                }
                if (*p == '}') ++p;
                else ok = false;
                return v;
            }

            if (*p == '[') {
                ++p;
                v.type = Json::Type::Array;
                skip();
                while (ok && *p && *p != ']') {
                    v.items.push_back(parseValue());
                    skip();
                }
                if (*p == ']') ++p;
                else ok = false;
                return v;
            }

            if (*p == '"') {
                v.type = Json::Type::String;
                v.text = parseString();
                return v;
            }

            if (std::strncmp(p, "true", 4) == 0) {
                p += 4;
                v.type = Json::Type::Bool;
                v.boolean = true;
                return v;
            }
            if (std::strncmp(p, "false", 5) == 0) {
                p += 5;
                v.type = Json::Type::Bool;
                v.boolean = false;
                return v;
            }
            if (std::strncmp(p, "null", 4) == 0) {
                p += 4;
                v.type = Json::Type::Null;
                return v;
            }

            char* end = nullptr;
            const double d = std::strtod(p, &end);
            if (end == p) { ok = false; return v; }
            p = end;
            v.type = Json::Type::Number;
            v.number = d;
            return v;
        }
    };

}  // namespace

const Json& Json::At(size_t index) const {
    if (type != Type::Array || index >= items.size()) return kNull;
    return items[index];
}

const Json& Json::Get(const char* key) const {
    if (type != Type::Object || key == nullptr) return kNull;
    for (const auto& kv : pairs) {
        if (kv.first == key) return kv.second;
    }
    return kNull;
}

bool Json::Has(const char* key) const {
    return !Get(key).IsNull();
}

float Json::AsFloat(float fallback) const {
    if (type == Type::Number) return (float)number;
    if (type == Type::Bool)   return boolean ? 1.0f : 0.0f;
    return fallback;
}

int Json::AsInt(int fallback) const {
    if (type == Type::Number) return (int)number;
    if (type == Type::Bool)   return boolean ? 1 : 0;
    return fallback;
}

bool Json::AsBool(bool fallback) const {
    if (type == Type::Bool)   return boolean;
    if (type == Type::Number) return number != 0.0;
    return fallback;
}

const char* Json::AsString(const char* fallback) const {
    if (type == Type::String) return text.c_str();
    return fallback;
}

Json ParseJson(const char* text) {
    if (text == nullptr) return Json{};
    Parser parser;
    parser.p = text;
    Json root = parser.parseValue();
    if (!parser.ok) return Json{};
    return root;
}

Json LoadJsonFile(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return Json{};

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return Json{}; }

    std::string buffer;
    buffer.resize((size_t)size + 1, '\0');
    const size_t read = std::fread(buffer.data(), 1, (size_t)size, f);
    std::fclose(f);
    buffer[read] = '\0';

    return ParseJson(buffer.c_str());
}

}  // namespace engine::data
