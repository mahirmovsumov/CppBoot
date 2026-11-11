#include "yaml.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace cppboot::config::detail {

namespace {

/// Deep enough for any real settings file, and shallow enough that recursion can't use up the stack.
constexpr std::size_t kMaxDepth = 64;

struct Line {
    std::size_t number{0};
    std::string text;
    /// Spaces before the content.
    std::size_t indent{0};
    /// Nothing but spaces, or a comment.
    bool blank{true};

    std::string_view content() const { return std::string_view{text}.substr(indent); }
};

struct ParseFailure {
    std::size_t line;
    std::string message;
};

[[noreturn]] void fail(std::size_t line, std::string message) {
    throw ParseFailure{line, std::move(message)};
}

bool isSpace(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trimLeft(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    return text;
}

std::string_view trimRight(std::string_view text) {
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Removes a comment, which starts with # at the start or after a space.
std::string_view withoutComment(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '#' && (i == 0 || isSpace(text[i - 1]))) {
            return trimRight(text.substr(0, i));
        }
    }
    return trimRight(text);
}

/// Checks that nothing but a comment follows a value that ended at position.
void expectEnd(std::string_view text, std::size_t position, std::size_t line) {
    const std::string_view rest = text.substr(position);
    if (!rest.empty() && !isSpace(rest.front())) {
        fail(line, "unexpected text after a value: " + std::string{rest});
    }
    if (!withoutComment(rest).empty()) {
        fail(line, "unexpected text after a value: " + std::string{trimLeft(rest)});
    }
}

void appendUtf8(std::string& out, std::uint32_t code, std::size_t line) {
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        fail(line, "invalid Unicode escape");
    }
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

std::uint32_t readHex(std::string_view text, std::size_t& position, std::size_t digits,
                      std::size_t line) {
    if (position + digits > text.size()) {
        fail(line, "an escape is cut short");
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const char c = text[position++];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            fail(line, "an escape has a character that isn't hexadecimal");
        }
    }
    return value;
}

/// Reads "..." starting at position, and leaves position after the closing quote.
std::string readDoubleQuoted(std::string_view text, std::size_t& position, std::size_t line) {
    std::string value;
    ++position;
    while (true) {
        if (position >= text.size()) {
            fail(line, "a quoted value has to end on the same line");
        }
        const char c = text[position++];
        if (c == '"') {
            return value;
        }
        if (c != '\\') {
            value += c;
            continue;
        }
        if (position >= text.size()) {
            fail(line, "a quoted value has to end on the same line");
        }
        const char escape = text[position++];
        switch (escape) {
            case '\\': value += '\\'; break;
            case '"': value += '"'; break;
            case '/': value += '/'; break;
            case ' ': value += ' '; break;
            case '0': value += '\0'; break;
            case 'a': value += '\a'; break;
            case 'b': value += '\b'; break;
            case 'e': value += '\x1b'; break;
            case 'f': value += '\f'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case 'v': value += '\v'; break;
            case 'x': appendUtf8(value, readHex(text, position, 2, line), line); break;
            case 'u': appendUtf8(value, readHex(text, position, 4, line), line); break;
            case 'U': appendUtf8(value, readHex(text, position, 8, line), line); break;
            default: fail(line, std::string{"unknown escape \\"} + escape);
        }
    }
}

/// Reads '...' starting at position, where '' stands for one quote.
std::string readSingleQuoted(std::string_view text, std::size_t& position, std::size_t line) {
    std::string value;
    ++position;
    while (true) {
        if (position >= text.size()) {
            fail(line, "a quoted value has to end on the same line");
        }
        const char c = text[position++];
        if (c != '\'') {
            value += c;
        } else if (position < text.size() && text[position] == '\'') {
            value += '\'';
            ++position;
        } else {
            return value;
        }
    }
}

std::string readQuoted(std::string_view text, std::size_t& position, std::size_t line) {
    return text[position] == '"' ? readDoubleQuoted(text, position, line)
                                 : readSingleQuoted(text, position, line);
}

/// Names what isn't supported, for a value that starts with one of YAML's indicators.
void rejectIndicator(char first, std::size_t line) {
    switch (first) {
        case '&':
        case '*':
            fail(line, "anchors and aliases (& and *) aren't supported in settings files");
        case '!':
            fail(line, "tags (!) aren't supported in settings files");
        case '?':
            fail(line, "complex keys (?) aren't supported in settings files");
        case '%':
            fail(line, "directives (%) aren't supported in settings files");
        case '@':
        case '`':
            fail(line, std::string{"a plain value can't start with "} + first + ". Put it in quotes");
        case '{':
            fail(line, "{...} maps aren't supported. Write one key per line instead");
        default:
            break;
    }
}

YamlNode scalar(std::string text) {
    YamlNode node;
    node.kind = YamlNode::Kind::Scalar;
    node.scalar = std::move(text);
    return node;
}

/// A plain value, which is null if it's empty or spells null.
YamlNode plainValue(std::string_view text, std::size_t line) {
    if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL") {
        return {};
    }
    rejectIndicator(text.front(), line);
    if (text.starts_with("- ") || text == "-") {
        fail(line, "a list item can't be a value on the same line as its key");
    }
    if (text.find(": ") != std::string_view::npos || text.back() == ':' ||
        text.find(":\t") != std::string_view::npos) {
        fail(line, "a value can't contain ': '. Put it in quotes");
    }
    return scalar(std::string{text});
}

/// Reads [a, 'b', "c"] on one line.
YamlNode flowSequence(std::string_view text, std::size_t line) {
    YamlNode node;
    node.kind = YamlNode::Kind::Sequence;
    std::size_t position = 1;
    while (true) {
        while (position < text.size() && isSpace(text[position])) {
            ++position;
        }
        if (position >= text.size()) {
            fail(line, "a [...] list has to end on the same line. Write one item per line with - "
                       "instead");
        }
        if (text[position] == ']') {
            ++position;
            break;
        }
        if (text[position] == '"' || text[position] == '\'') {
            node.items.push_back(scalar(readQuoted(text, position, line)));
        } else {
            const std::size_t start = position;
            while (position < text.size() && text[position] != ',' && text[position] != ']') {
                if (text[position] == '[' || text[position] == '{' || text[position] == '}' ||
                    text[position] == '#') {
                    fail(line, "a [...] list can only hold plain or quoted values");
                }
                ++position;
            }
            const std::string_view item = trimRight(text.substr(start, position - start));
            if (item.empty()) {
                fail(line, "a [...] list has an empty item");
            }
            node.items.push_back(plainValue(item, line));
        }
        while (position < text.size() && isSpace(text[position])) {
            ++position;
        }
        if (position >= text.size()) {
            fail(line, "a [...] list has to end on the same line. Write one item per line with - "
                       "instead");
        }
        if (text[position] == ',') {
            ++position;
        } else if (text[position] != ']') {
            fail(line, "expected , or ] in a [...] list");
        }
    }
    expectEnd(text, position, line);
    return node;
}

/// A value written on the same line as its key or its -.
YamlNode inlineValue(std::string_view text, std::size_t line) {
    text = trimLeft(text);
    if (text.empty()) {
        return {};
    }
    if (text.front() == '"' || text.front() == '\'') {
        std::size_t position = 0;
        YamlNode node = scalar(readQuoted(text, position, line));
        expectEnd(text, position, line);
        return node;
    }
    if (text.front() == '[') {
        return flowSequence(text, line);
    }
    if (withoutComment(text) == "{}") {
        YamlNode node;
        node.kind = YamlNode::Kind::Map;
        return node;
    }
    return plainValue(withoutComment(text), line);
}

struct KeyAndRest {
    std::string key;
    /// Everything after the colon.
    std::string_view rest;
};

/// Splits "key: value", or returns nothing if the text isn't a key.
std::optional<KeyAndRest> splitKey(std::string_view text, std::size_t line) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '"' || text.front() == '\'') {
        std::size_t position = 0;
        std::string key = readQuoted(text, position, line);
        while (position < text.size() && isSpace(text[position])) {
            ++position;
        }
        if (position < text.size() && text[position] == ':' &&
            (position + 1 == text.size() || isSpace(text[position + 1]))) {
            return KeyAndRest{std::move(key), text.substr(position + 1)};
        }
        return std::nullopt;
    }
    if (std::string_view{"[{#&*!|>%@`?"}.find(text.front()) != std::string_view::npos) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '#' && i > 0 && isSpace(text[i - 1])) {
            return std::nullopt;
        }
        if (text[i] == ':' && (i + 1 == text.size() || isSpace(text[i + 1]))) {
            const std::string_view key = trimRight(text.substr(0, i));
            if (key.empty()) {
                return std::nullopt;
            }
            return KeyAndRest{std::string{key}, text.substr(i + 1)};
        }
    }
    return std::nullopt;
}

bool isSequenceItem(const Line& line) {
    const std::string_view content = line.content();
    return content == "-" || content.starts_with("- ") || content.starts_with("-\t");
}

bool isDocumentMarker(const Line& line) {
    const std::string_view content = withoutComment(line.content());
    return line.indent == 0 && (content == "---" || content == "...");
}

class Parser {
public:
    explicit Parser(std::string_view text) {
        if (text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
        }
        std::size_t number = 1;
        while (true) {
            const std::size_t end = text.find('\n');
            std::string_view raw = text.substr(0, end);
            if (raw.ends_with('\r')) {
                raw.remove_suffix(1);
            }
            addLine(raw, number++);
            if (end == std::string_view::npos) {
                break;
            }
            text.remove_prefix(end + 1);
        }
    }

    YamlNode parse() {
        skipBlank();
        if (!atEnd() && isDocumentMarker(current()) && withoutComment(current().content()) == "---") {
            ++next_;
            skipBlank();
        }
        if (atEnd()) {
            return {};
        }
        YamlNode root = parseBlock(current().indent, 0);
        skipBlank();
        if (!atEnd()) {
            fail(current().number, "this line is indented less than the first one");
        }
        return root;
    }

private:
    void addLine(std::string_view raw, std::size_t number) {
        Line line;
        line.number = number;
        line.text = std::string{raw};
        while (line.indent < raw.size() && raw[line.indent] == ' ') {
            ++line.indent;
        }
        const std::string_view rest = raw.substr(line.indent);
        line.blank = trimLeft(rest).empty() || trimLeft(rest).front() == '#';
        if (!line.blank && rest.front() == '\t') {
            fail(number, "tabs can't indent. Use spaces");
        }
        lines_.push_back(std::move(line));
    }

    bool atEnd() const { return next_ >= lines_.size(); }
    const Line& current() const { return lines_[next_]; }

    void skipBlank() {
        while (!atEnd() && current().blank) {
            ++next_;
        }
    }

    void checkLine(const Line& line) const {
        if (isDocumentMarker(line)) {
            fail(line.number, "a settings file can hold only one document");
        }
    }

    YamlNode parseBlock(std::size_t indent, std::size_t depth) {
        if (depth > kMaxDepth) {
            fail(current().number, "settings are nested too deeply");
        }
        return isSequenceItem(current()) ? parseSequence(indent, depth) : parseMapping(indent, depth);
    }

    YamlNode parseMapping(std::size_t indent, std::size_t depth) {
        YamlNode node;
        node.kind = YamlNode::Kind::Map;
        while (true) {
            skipBlank();
            if (atEnd() || current().indent < indent) {
                break;
            }
            const Line line = current();
            checkLine(line);
            if (line.indent > indent) {
                fail(line.number, "unexpected indentation. A value has to fit on one line; use | "
                                  "for text over several lines");
            }
            if (isSequenceItem(line)) {
                fail(line.number, "a list item where a key was expected");
            }
            if (!line.content().empty()) {
                rejectIndicator(line.content().front(), line.number);
            }
            auto split = splitKey(line.content(), line.number);
            if (!split) {
                fail(line.number, "expected 'key: value'");
            }
            const bool duplicate = std::any_of(
                node.entries.begin(), node.entries.end(),
                [&](const YamlNode::Entry& entry) { return entry.key == split->key; });
            if (duplicate) {
                fail(line.number, "the key '" + split->key + "' appears twice");
            }
            ++next_;
            YamlNode value = valueAfter(split->rest, line, indent, depth, true);
            node.entries.push_back(YamlNode::Entry{std::move(split->key), std::move(value)});
        }
        return node;
    }

    YamlNode parseSequence(std::size_t indent, std::size_t depth) {
        YamlNode node;
        node.kind = YamlNode::Kind::Sequence;
        while (true) {
            skipBlank();
            if (atEnd() || current().indent < indent || !isSequenceItem(current())) {
                break;
            }
            const Line line = current();
            if (line.indent > indent) {
                fail(line.number, "unexpected indentation in a list");
            }
            const std::string_view after_dash = line.content().substr(1);
            if (!after_dash.empty() && after_dash.front() == '\t') {
                fail(line.number, "tabs can't indent. Use spaces");
            }
            const std::string_view item = trimLeft(after_dash);
            if (item == "-" || item.starts_with("- ")) {
                fail(line.number, "lists inside lists aren't supported");
            }

            if (splitKey(item, line.number)) {
                // "- key: value" starts a map whose keys line up with this one. Blanking the dash
                // lets the map read this line like any other.
                const std::size_t column = line.indent + 1 + (after_dash.size() - item.size());
                if (depth + 1 > kMaxDepth) {
                    fail(line.number, "settings are nested too deeply");
                }
                lines_[next_].text[line.indent] = ' ';
                lines_[next_].indent = column;
                node.items.push_back(parseMapping(column, depth + 1));
            } else {
                ++next_;
                node.items.push_back(valueAfter(item, line, indent, depth, false));
            }
        }
        return node;
    }

    /// The value after a key's colon or a list's dash, which is either on the same line or in the
    /// indented lines below.
    YamlNode valueAfter(std::string_view rest, const Line& line, std::size_t indent,
                        std::size_t depth, bool list_may_share_indent) {
        rest = trimLeft(rest);
        if (!rest.empty() && (rest.front() == '|' || rest.front() == '>')) {
            return blockScalar(withoutComment(rest), line, indent);
        }
        if (!withoutComment(rest).empty()) {
            return inlineValue(rest, line.number);
        }

        skipBlank();
        if (atEnd()) {
            return {};
        }
        const Line& below = current();
        if (below.indent > indent) {
            return parseBlock(below.indent, depth + 1);
        }
        // YAML lets a list sit at the same indentation as its key.
        if (list_may_share_indent && below.indent == indent && isSequenceItem(below)) {
            return parseSequence(indent, depth + 1);
        }
        return {};
    }

    /// Reads a | or > block from the lines indented below its key.
    YamlNode blockScalar(std::string_view header, const Line& line, std::size_t parent_indent) {
        const std::string_view chomping = header.substr(1);
        if (chomping != "" && chomping != "-" && chomping != "+") {
            fail(line.number, "only |, |-, |+, >, >- and >+ are supported");
        }
        const bool folded = header.front() == '>';

        std::vector<std::string_view> pieces;
        std::size_t content_indent = 0;
        while (!atEnd()) {
            const Line& next = current();
            const bool empty = trimLeft(next.text).empty();
            if (!empty && next.indent <= parent_indent) {
                break;
            }
            if (!empty && content_indent == 0) {
                content_indent = next.indent;
            }
            if (!empty && next.indent < content_indent) {
                fail(next.number, "this line is indented less than the first line of its text");
            }
            pieces.push_back(empty ? std::string_view{}
                                   : std::string_view{next.text}.substr(content_indent));
            ++next_;
        }

        std::size_t trailing = 0;
        while (!pieces.empty() && pieces.back().empty()) {
            pieces.pop_back();
            ++trailing;
        }

        std::string text;
        bool previous_had_text = false;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (!folded) {
                text += i == 0 ? "" : "\n";
                text += pieces[i];
            } else if (pieces[i].empty()) {
                text += '\n';
                previous_had_text = false;
            } else {
                text += previous_had_text ? " " : "";
                text += pieces[i];
                previous_had_text = true;
            }
        }
        if (chomping == "+") {
            text += std::string(pieces.empty() ? trailing : trailing + 1, '\n');
        } else if (chomping == "" && !pieces.empty()) {
            text += '\n';
        }
        return scalar(std::move(text));
    }

    std::vector<Line> lines_;
    std::size_t next_{0};
};

}  // namespace

Result<YamlNode> parseYaml(std::string_view text) {
    try {
        Parser parser{text};
        return parser.parse();
    } catch (const ParseFailure& failure) {
        return Error{ErrorCode::ParseError,
                     "line " + std::to_string(failure.line) + ": " + failure.message};
    }
}

}  // namespace cppboot::config::detail
