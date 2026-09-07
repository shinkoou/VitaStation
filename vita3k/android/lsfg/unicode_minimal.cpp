#include <pe-parse/to_string.h>

#include <cstdint>
#include <string>

namespace peparse {

std::string from_utf16(const UCharString& input) {
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(input[i]);

        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < input.size()) {
            const std::uint32_t low = static_cast<std::uint16_t>(input[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10u) + (low - 0xDC00u);
                ++i;
            }
        }

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    return out;
}

} // namespace peparse
