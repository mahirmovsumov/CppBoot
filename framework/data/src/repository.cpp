#include "cppboot/data/repository.hpp"

namespace cppboot::data {

std::string likePattern(std::string_view text, TextMatch match) {
    std::string pattern;
    pattern.reserve(text.size() + 2);
    if (match != TextMatch::StartsWith) {
        pattern += '%';
    }
    for (const char c : text) {
        if (c == '%' || c == '_' || c == '\\') {
            pattern += '\\';
        }
        pattern += c;
    }
    if (match != TextMatch::EndsWith) {
        pattern += '%';
    }
    return pattern;
}

}  // namespace cppboot::data
