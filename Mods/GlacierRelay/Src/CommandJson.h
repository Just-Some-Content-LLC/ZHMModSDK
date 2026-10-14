#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A strict, bounded JSON reader for the command link (M5-B, glacier-relay ADR 0009). The adapter
// had no parser before M5-B because it read nothing; the command link reads lines from BEAM, so
// it needs exactly this much: objects, arrays, strings, numbers, booleans and null, with the
// limits below, no extensions, no duplicate keys, valid UTF-8, control characters escaped. It
// never touches the engine and is engine-independent (built into the tests without the SDK).
//
// Values are plain owned data. A failed parse names the reason; nothing is "best effort".
namespace CommandJson
{
    constexpr size_t k_MaxDepth = 4;          // object/array nesting (the schema needs 3)
    constexpr size_t k_MaxNodes = 256;        // values in one document
    constexpr size_t k_MaxStringBytes = 1024; // one string value or key

    struct Value
    {
        enum class Kind
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object,
        };

        Kind kind = Kind::Null;
        bool boolean = false;
        double number = 0.0;
        bool is_integer = false; // the number was written without fraction or exponent and fits int64
        int64_t integer = 0;
        std::string text;
        std::vector<Value> items;
        std::vector<std::pair<std::string, Value>> fields; // source order, unique keys

        const Value* Find(std::string_view p_Key) const;
    };

    struct ParseResult
    {
        bool ok = false;
        std::string error; // empty when ok
        Value value;
    };

    ParseResult Parse(std::string_view p_Text);

    // True when p_Bytes is well-formed UTF-8 (no overlongs, no surrogates, no code points > U+10FFFF).
    bool ValidUtf8(std::string_view p_Bytes);
}
