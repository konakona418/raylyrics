#include "lyrics/encoding.h"

#include <iconv.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace raylyrics {

namespace {

bool IsValidUtf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        if (c < 0x80) {
            length = 1;
        } else if ((c & 0xE0) == 0xC0) {
            length = 2;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t k = 1; k < length; k++) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += length;
    }
    return true;
}

std::string Convert(const std::string& input, const char* from) {
    iconv_t converter = iconv_open("UTF-8", from);
    if (converter == reinterpret_cast<iconv_t>(-1)) return {};

    std::string output(input.size() * 4 + 16, '\0');
    char* input_ptr = const_cast<char*>(input.data());
    std::size_t input_left = input.size();
    char* output_ptr = output.data();
    std::size_t output_left = output.size();

    while (input_left > 0) {
        const std::size_t result = iconv(converter, &input_ptr, &input_left, &output_ptr, &output_left);
        if (result == static_cast<std::size_t>(-1)) {
            if (errno == E2BIG) {
                const std::size_t used = output.size() - output_left;
                output.resize(output.size() * 2);
                output_ptr = output.data() + used;
                output_left = output.size() - used;
                continue;
            }
            iconv_close(converter);
            return {};
        }
    }

    iconv_close(converter);
    output.resize(output.size() - output_left);
    return output;
}

}  // namespace

std::string ToUtf8(const std::string& bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return bytes.substr(3);
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::string converted = Convert(bytes.substr(2), "UTF-16LE");
        if (!converted.empty()) return converted;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        std::string converted = Convert(bytes.substr(2), "UTF-16BE");
        if (!converted.empty()) return converted;
    }

    if (IsValidUtf8(bytes)) return bytes;

    std::string converted = Convert(bytes, "GB18030");
    if (!converted.empty()) return converted;
    return bytes;
}

}  // namespace raylyrics
