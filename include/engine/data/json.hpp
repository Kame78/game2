#pragma once
#include <string>
#include <utility>
#include <vector>

// Minimal read-only JSON value tree — enough for data-driven gameplay assets
// (objects, arrays, numbers, strings, bools, null). Tolerant of trailing commas
// and // line comments so designers can annotate data files.
namespace engine::data {

    class Json {
    public:
        enum class Type : uint8_t { Null = 0, Bool, Number, String, Array, Object };

        Type type = Type::Null;
        bool boolean = false;
        double number = 0.0;
        std::string text;
        std::vector<Json> items;                        // Array
        std::vector<std::pair<std::string, Json>> pairs; // Object

        bool IsNull()   const { return type == Type::Null; }
        bool IsObject() const { return type == Type::Object; }
        bool IsArray()  const { return type == Type::Array; }

        size_t Size() const {
            if (type == Type::Array)  return items.size();
            if (type == Type::Object) return pairs.size();
            return 0;
        }

        // Array element (returns a shared null value when out of range).
        const Json& At(size_t index) const;
        // Object member by key (returns a shared null value when missing).
        const Json& Get(const char* key) const;
        bool Has(const char* key) const;

        float       AsFloat(float fallback = 0.0f) const;
        int         AsInt(int fallback = 0) const;
        bool        AsBool(bool fallback = false) const;
        const char* AsString(const char* fallback = "") const;

        // Convenience: member lookup + conversion in one call.
        float       Float(const char* key, float fallback = 0.0f) const { return Get(key).AsFloat(fallback); }
        int         Int(const char* key, int fallback = 0) const { return Get(key).AsInt(fallback); }
        bool        Bool(const char* key, bool fallback = false) const { return Get(key).AsBool(fallback); }
        const char* Str(const char* key, const char* fallback = "") const { return Get(key).AsString(fallback); }
    };

    // Parse text. Returns a Null value on malformed input.
    Json ParseJson(const char* text);
    // Load + parse a file. Returns a Null value when the file is missing.
    Json LoadJsonFile(const char* path);

}  // namespace engine::data
