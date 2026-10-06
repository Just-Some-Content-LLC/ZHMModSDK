#pragma once

#include <string>
#include <string_view>

// The little JSON the adapter needs: quoting strings for hand-built objects. Output is UTF-8 with
// the characters JSON requires escaped. No parser; the adapter reads nothing in M1.
namespace Json
{
    // Returns the quoted, escaped JSON string literal for p_Value.
    std::string Quote(std::string_view p_Value);
}
