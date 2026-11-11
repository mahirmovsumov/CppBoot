#include <gtest/gtest.h>

#include <cppboot/config/config.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>

#include "yaml.hpp"

using cppboot::config::detail::parseYaml;
using cppboot::config::detail::YamlNode;

namespace {

/// Flattens like Config does, so tests can compare whole files in one line.
void flatten(const YamlNode& node, const std::string& prefix, std::map<std::string, std::string>& out) {
    switch (node.kind) {
        case YamlNode::Kind::Map:
            for (const auto& [key, value] : node.entries) {
                flatten(value, prefix.empty() ? key : prefix + "." + key, out);
            }
            break;
        case YamlNode::Kind::Sequence:
            for (std::size_t i = 0; i < node.items.size(); ++i) {
                flatten(node.items[i], prefix + "." + std::to_string(i), out);
            }
            break;
        case YamlNode::Kind::Scalar:
            out[prefix] = node.scalar;
            break;
        case YamlNode::Kind::Null:
            out[prefix] = "<null>";
            break;
    }
}

std::map<std::string, std::string> read(std::string_view text) {
    auto node = parseYaml(text);
    EXPECT_TRUE(node) << node.error().message() << "\nin:\n" << text;
    std::map<std::string, std::string> values;
    if (node) {
        flatten(node.value(), {}, values);
    }
    return values;
}

/// The error for text that has to be rejected.
std::string rejected(std::string_view text) {
    auto node = parseYaml(text);
    EXPECT_FALSE(node) << "accepted:\n" << text;
    return node ? std::string{} : node.error().message();
}

using Values = std::map<std::string, std::string>;

}  // namespace

TEST(YamlTest, ReadsNestedKeys) {
    EXPECT_EQ(read("cppboot:\n  server:\n    port: 8080\n    host: 127.0.0.1\n  name: shop\n"),
              (Values{{"cppboot.server.port", "8080"},
                      {"cppboot.server.host", "127.0.0.1"},
                      {"cppboot.name", "shop"}}));
}

TEST(YamlTest, IgnoresCommentsAndBlankLines) {
    EXPECT_EQ(read("# settings\n\na: 1 # one\n  # indented comment\nb: x#y\n"),
              (Values{{"a", "1"}, {"b", "x#y"}}));
}

TEST(YamlTest, KeepsUrlsAndColonsWithoutSpaces) {
    EXPECT_EQ(read("url: postgresql://user:pw@localhost:5432/db?x=1\ntime: 12:30\n"),
              (Values{{"url", "postgresql://user:pw@localhost:5432/db?x=1"}, {"time", "12:30"}}));
}

TEST(YamlTest, ReadsQuotedValuesAndEscapes) {
    EXPECT_EQ(read(R"(a: "x: y # not a comment"
b: 'it''s'
c: "tab\there \u00e9 \x41 \"q\""
d: ""
"quoted key": 1
)"),
              (Values{{"a", "x: y # not a comment"},
                      {"b", "it's"},
                      {"c", "tab\there \xC3\xA9 A \"q\""},
                      {"d", ""},
                      {"quoted key", "1"}}));
}

TEST(YamlTest, ReadsNullsAsNull) {
    EXPECT_EQ(read("a:\nb: ~\nc: null\nd: 'null'\n"),
              (Values{{"a", "<null>"}, {"b", "<null>"}, {"c", "<null>"}, {"d", "null"}}));
}

TEST(YamlTest, ReadsBlockLists) {
    EXPECT_EQ(read("hosts:\n  - one\n  - 'two'\nsame_indent:\n- a\n- b\nafter: 1\n"),
              (Values{{"hosts.0", "one"},
                      {"hosts.1", "two"},
                      {"same_indent.0", "a"},
                      {"same_indent.1", "b"},
                      {"after", "1"}}));
}

TEST(YamlTest, ReadsListsOfMaps) {
    EXPECT_EQ(read("routes:\n  - path: /a\n    limit: 5\n  -   path: /b\n      nested:\n        deep: x\n"),
              (Values{{"routes.0.path", "/a"},
                      {"routes.0.limit", "5"},
                      {"routes.1.path", "/b"},
                      {"routes.1.nested.deep", "x"}}));
}

TEST(YamlTest, ReadsInlineLists) {
    EXPECT_EQ(read("a: [10.0.0.1, '10.0.0.0/8', \"x, y\"]\nb: []\nc: [one,] # comment\nd: {}\n"),
              (Values{{"a.0", "10.0.0.1"}, {"a.1", "10.0.0.0/8"}, {"a.2", "x, y"}, {"c.0", "one"}}));
}

TEST(YamlTest, ReadsTextBlocks) {
    EXPECT_EQ(read("a: |\n  line one\n    indented\n\n  last\nb: >\n  folded\n  text\n\n  next\n"
                   "c: |-\n  no end\nd: |+\n  keep\n\n\ne: x\n"),
              (Values{{"a", "line one\n  indented\n\nlast\n"},
                      {"b", "folded text\nnext\n"},
                      {"c", "no end"},
                      {"d", "keep\n\n\n"},
                      {"e", "x"}}));
}

TEST(YamlTest, AcceptsWindowsLineEndingsAByteOrderMarkAndOneDocumentMarker) {
    EXPECT_EQ(read("\xEF\xBB\xBF---\r\na: 1\r\nb:\r\n  c: 2\r\n"),
              (Values{{"a", "1"}, {"b.c", "2"}}));
}

TEST(YamlTest, EmptyTextIsNull) {
    EXPECT_EQ(parseYaml("").value().kind, YamlNode::Kind::Null);
    EXPECT_EQ(parseYaml("# only a comment\n\n").value().kind, YamlNode::Kind::Null);
}

TEST(YamlTest, NamesTheLineOfAnError) {
    EXPECT_EQ(rejected("a: 1\nb: 2\nc: [x\n"),
              "line 3: a [...] list has to end on the same line. Write one item per line with - "
              "instead");
}

TEST(YamlTest, RejectsWhatItDoesNotSupport) {
    EXPECT_NE(rejected("a: &x 1\nb: *x\n").find("anchors"), std::string::npos);
    EXPECT_NE(rejected("a: !!str 1\n").find("tags"), std::string::npos);
    EXPECT_NE(rejected("a: {b: 1}\n").find("maps aren't supported"), std::string::npos);
    EXPECT_NE(rejected("? a\n: b\n").find("complex keys"), std::string::npos);
    EXPECT_NE(rejected("%YAML 1.2\na: 1\n").find("directives"), std::string::npos);
    EXPECT_NE(rejected("a: 1\n---\nb: 2\n").find("one document"), std::string::npos);
    EXPECT_NE(rejected("a:\n  - - x\n").find("lists inside lists"), std::string::npos);
    EXPECT_NE(rejected("a: |2\n  x\n").find("only |"), std::string::npos);
}

TEST(YamlTest, RejectsMistakesRatherThanGuessing) {
    // A plain value over two lines.
    EXPECT_NE(rejected("a: one\n  two\n").find("unexpected indentation"), std::string::npos);
    EXPECT_NE(rejected("a: 1\na: 2\n").find("appears twice"), std::string::npos);
    EXPECT_NE(rejected("a: b: c\n").find("': '"), std::string::npos);
    EXPECT_NE(rejected("a:\n\tb: 1\n").find("tabs"), std::string::npos);
    EXPECT_NE(rejected("just text\n").find("key: value"), std::string::npos);
    EXPECT_NE(rejected("a: \"open\n").find("same line"), std::string::npos);
    EXPECT_NE(rejected("a: 'x' y\n").find("after a value"), std::string::npos);
    EXPECT_NE(rejected("a: \"\\q\"\n").find("unknown escape"), std::string::npos);
    EXPECT_NE(rejected("a: \"\\ud800\"\n").find("Unicode"), std::string::npos);
    EXPECT_NE(rejected("  a: 1\nb: 2\n").find("indented less"), std::string::npos);
    EXPECT_NE(rejected("a: [x, , y]\n").find("empty item"), std::string::npos);
}

TEST(YamlTest, LimitsNesting) {
    std::string deep;
    for (int i = 0; i < 200; ++i) {
        deep += std::string(static_cast<std::size_t>(i) * 2, ' ') + "k:\n";
    }
    EXPECT_NE(rejected(deep).find("nested too deeply"), std::string::npos);

    std::string list;
    for (int i = 0; i < 200; ++i) {
        list += std::string(static_cast<std::size_t>(i) * 2, ' ') + "- k:\n";
    }
    EXPECT_FALSE(parseYaml(list));
}

// Real settings files are trusted, but the parser still has to fail cleanly on anything, never crash
// or hang. This mutates valid files at random, with a fixed seed so a failure can be repeated. The
// sanitizer builds in CI run it too, and the fuzz job goes much further.
TEST(YamlTest, SurvivesDamagedInput) {
    const std::string samples[] = {
        "cppboot:\n  server:\n    port: 8080\n    trusted_proxies: [10.0.0.1, '::1']\n",
        "routes:\n  - path: /a\n    limit: 5\n  - b\ntext: |\n  one\n  two\n",
        "a: \"\\u00e9\\x41\"\nb: 'it''s'\nc: >-\n  folded\n\n  text\n",
    };
    const std::string alphabet = " \t\n\r-:#[]{},'\"|>&*!?%@`\\ux0aZ";
    std::mt19937 random{12345};
    for (int round = 0; round < 20000; ++round) {
        std::string text = samples[static_cast<std::size_t>(round) % std::size(samples)];
        const int edits = 1 + static_cast<int>(random() % 8);
        for (int e = 0; e < edits && !text.empty(); ++e) {
            const std::size_t at = random() % text.size();
            switch (random() % 3) {
                case 0: text[at] = alphabet[random() % alphabet.size()]; break;
                case 1: text.insert(at, 1, alphabet[random() % alphabet.size()]); break;
                default: text.erase(at, 1); break;
            }
        }
        static_cast<void>(parseYaml(text));
    }
}

// Every settings file in the repository, and every yaml block in the documentation, has to read
// with this parser, since that's what people copy.
TEST(YamlTest, ReadsEveryExampleInTheRepository) {
    namespace fs = std::filesystem;
    const fs::path source{CPPBOOT_SOURCE_DIR};
    int checked = 0;
    for (const char* folder : {"examples", "docs", "benchmarks"}) {
        for (const auto& entry : fs::recursive_directory_iterator{source / folder}) {
            const fs::path& path = entry.path();
            std::ifstream input{path, std::ios::binary};
            std::stringstream content;
            content << input.rdbuf();
            const std::string text = content.str();

            if (path.extension() == ".yaml") {
                const auto node = parseYaml(text);
                EXPECT_TRUE(node) << path.generic_string() << ": " << node.error().message();
                ++checked;
            } else if (path.extension() == ".md") {
                std::size_t start = 0;
                while ((start = text.find("```yaml\n", start)) != std::string::npos) {
                    start += 8;
                    const std::size_t end = text.find("```", start);
                    const auto node = parseYaml(text.substr(start, end - start));
                    EXPECT_TRUE(node) << path.generic_string() << ": " << node.error().message()
                                      << "\n" << text.substr(start, end - start);
                    ++checked;
                    start = end;
                }
            }
        }
    }
    // So that moving the documentation can't silently leave nothing checked.
    EXPECT_GE(checked, 9);
}
