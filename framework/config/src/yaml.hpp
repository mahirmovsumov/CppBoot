#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::config::detail {

struct YamlNode {
    enum class Kind { Null, Scalar, Map, Sequence };
    /// Defined below, where YamlNode is complete, which Clang needs for the vector.
    struct Entry;

    Kind kind{Kind::Null};
    std::string scalar;
    /// A map's keys and values, in the order written.
    std::vector<Entry> entries;
    std::vector<YamlNode> items;
};

struct YamlNode::Entry {
    std::string key;
    YamlNode value;
};

/// Reads the part of YAML that settings files use: nested keys, lists written with - or [a, b],
/// plain and quoted values, | and > text blocks, and comments. Anything else, such as anchors,
/// tags, {a: 1} or a plain value over several lines, is an error that names the line, rather than
/// something read differently from what was meant. So is a key written twice in one map.
Result<YamlNode> parseYaml(std::string_view text);

}  // namespace cppboot::config::detail
