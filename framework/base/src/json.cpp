#include "cppboot/base/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>
#include <utility>

namespace cppboot {

namespace {

/// Deep enough for any real document, and shallow enough that recursion can't use up the stack.
constexpr std::size_t kMaxDepth = 128;

std::string_view typeName(Json::Type type) {
    switch (type) {
        case Json::Type::Null:
            return "null";
        case Json::Type::Bool:
            return "true or false";
        case Json::Type::Number:
            return "a number";
        case Json::Type::String:
            return "a string";
        case Json::Type::Array:
            return "an array";
        case Json::Type::Object:
            return "an object";
    }
    return "a value";
}

[[noreturn]] void wrongType(Json::Type expected, Json::Type actual) {
    throw JsonError("expected " + std::string{typeName(expected)} + ", but found " +
                    std::string{typeName(actual)});
}

/// The length of the UTF-8 sequence at position, or 0 if it isn't valid: overlong forms,
/// surrogates and code points above U+10FFFF are all rejected.
std::size_t utf8Length(std::string_view text, std::size_t position) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[position + i]); };
    const auto continuation = [&](std::size_t i) {
        return position + i < text.size() && (byte(i) & 0xC0) == 0x80;
    };
    const unsigned char first = byte(0);
    if (first < 0x80) {
        return 1;
    }
    if (first >= 0xC2 && first <= 0xDF) {
        return continuation(1) ? 2 : 0;
    }
    if (first >= 0xE0 && first <= 0xEF) {
        if (!continuation(1) || !continuation(2)) {
            return 0;
        }
        if ((first == 0xE0 && byte(1) < 0xA0) || (first == 0xED && byte(1) >= 0xA0)) {
            return 0;
        }
        return 3;
    }
    if (first >= 0xF0 && first <= 0xF4) {
        if (!continuation(1) || !continuation(2) || !continuation(3)) {
            return 0;
        }
        if ((first == 0xF0 && byte(1) < 0x90) || (first == 0xF4 && byte(1) >= 0x90)) {
            return 0;
        }
        return 4;
    }
    return 0;
}

void appendUtf8(std::string& out, std::uint32_t code) {
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

void dumpString(std::string_view text, std::string& out) {
    constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    std::size_t position = 0;
    while (position < text.size()) {
        const auto c = static_cast<unsigned char>(text[position]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 0xF];
                } else if (const std::size_t length = utf8Length(text, position); length == 0) {
                    out += "\xEF\xBF\xBD";
                } else {
                    out += text.substr(position, length);
                    position += length;
                    continue;
                }
        }
        ++position;
    }
    out += '"';
}

}  // namespace

/// Reads JSON as RFC 8259 describes it, and nothing more lenient.
class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    Json parseDocument() {
        Json value = parseValue(0);
        skipSpace();
        if (position_ != text_.size()) {
            fail("unexpected text after the JSON value");
        }
        return value;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw JsonError(std::string{message} + " at offset " + std::to_string(position_));
    }

    void skipSpace() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' ||
                text_[position_] == '\r')) {
            ++position_;
        }
    }

    bool consume(std::string_view word) {
        if (text_.substr(position_, word.size()) == word) {
            position_ += word.size();
            return true;
        }
        return false;
    }

    Json parseValue(std::size_t depth) {
        skipSpace();
        if (position_ >= text_.size()) {
            fail("unexpected end of JSON");
        }
        switch (text_[position_]) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': return Json{parseString()};
            case 't':
                if (consume("true")) return Json{true};
                break;
            case 'f':
                if (consume("false")) return Json{false};
                break;
            case 'n':
                if (consume("null")) return Json{};
                break;
            default:
                if (text_[position_] == '-' || (text_[position_] >= '0' && text_[position_] <= '9')) {
                    return parseNumber();
                }
        }
        fail("invalid JSON value");
    }

    Json parseObject(std::size_t depth) {
        if (depth >= kMaxDepth) {
            fail("JSON is nested too deeply");
        }
        ++position_;
        Json result = Json::object();
        auto& members = std::get<Json::Object>(result.value_);
        skipSpace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return result;
        }
        while (true) {
            skipSpace();
            if (position_ >= text_.size() || text_[position_] != '"') {
                fail("expected a key in quotes");
            }
            std::string key = parseString();
            skipSpace();
            if (position_ >= text_.size() || text_[position_] != ':') {
                fail("expected ':' after a key");
            }
            ++position_;
            Json value = parseValue(depth + 1);
            members.push_back(JsonMember{std::move(key), std::move(value)});
            skipSpace();
            if (position_ < text_.size() && text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (position_ < text_.size() && text_[position_] == '}') {
                ++position_;
                break;
            }
            fail("expected ',' or '}' in an object");
        }

        // Sorting views of the keys finds a repeat in n log n, so a large body can't make this slow.
        std::vector<std::string_view> keys;
        keys.reserve(members.size());
        for (const JsonMember& member : members) {
            keys.push_back(member.key);
        }
        std::sort(keys.begin(), keys.end());
        if (const auto repeat = std::adjacent_find(keys.begin(), keys.end()); repeat != keys.end()) {
            throw JsonError("the key '" + std::string{*repeat} + "' appears twice in an object");
        }
        return result;
    }

    Json parseArray(std::size_t depth) {
        if (depth >= kMaxDepth) {
            fail("JSON is nested too deeply");
        }
        ++position_;
        Json result = Json::array();
        auto& items = std::get<Json::Array>(result.value_);
        skipSpace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return result;
        }
        while (true) {
            items.push_back(parseValue(depth + 1));
            skipSpace();
            if (position_ < text_.size() && text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (position_ < text_.size() && text_[position_] == ']') {
                ++position_;
                return result;
            }
            fail("expected ',' or ']' in an array");
        }
    }

    std::uint32_t parseHex4() {
        if (position_ + 4 > text_.size()) {
            fail("a \\u escape is cut short");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("a \\u escape has a character that isn't hexadecimal");
            }
        }
        return value;
    }

    std::string parseString() {
        ++position_;
        std::string value;
        while (true) {
            if (position_ >= text_.size()) {
                fail("a string has no closing quote");
            }
            const auto c = static_cast<unsigned char>(text_[position_]);
            if (c == '"') {
                ++position_;
                return value;
            }
            if (c < 0x20) {
                fail("a string contains a control character, which must be escaped");
            }
            if (c != '\\') {
                const std::size_t length = utf8Length(text_, position_);
                if (length == 0) {
                    fail("a string isn't valid UTF-8");
                }
                value += text_.substr(position_, length);
                position_ += length;
                continue;
            }

            ++position_;
            if (position_ >= text_.size()) {
                fail("a string has no closing quote");
            }
            const char escape = text_[position_++];
            switch (escape) {
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'u': {
                    std::uint32_t code = parseHex4();
                    if (code >= 0xDC00 && code <= 0xDFFF) {
                        fail("a \\u escape has a low surrogate on its own");
                    }
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (!consume("\\u")) {
                            fail("a \\u escape has a high surrogate on its own");
                        }
                        const std::uint32_t low = parseHex4();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            fail("a \\u escape has a high surrogate on its own");
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(value, code);
                    break;
                }
                default:
                    fail("a string has an unknown escape");
            }
        }
    }

    Json parseNumber() {
        const std::size_t start = position_;
        const auto digits = [&] {
            const std::size_t first = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            return position_ - first;
        };

        if (text_[position_] == '-') {
            ++position_;
        }
        const std::size_t whole_start = position_;
        const std::size_t whole = digits();
        if (whole == 0) {
            fail("invalid number");
        }
        if (whole > 1 && text_[whole_start] == '0') {
            fail("a number can't start with 0");
        }
        bool integer = true;
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            integer = false;
            if (digits() == 0) {
                fail("invalid number");
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            integer = false;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (digits() == 0) {
                fail("invalid number");
            }
        }

        const char* const first = text_.data() + start;
        const char* const last = text_.data() + position_;
        if (integer) {
            std::int64_t value = 0;
            if (const auto [end, error] = std::from_chars(first, last, value);
                error == std::errc{} && end == last) {
                return Json::fromInteger(value);
            }
            std::uint64_t positive = 0;
            if (const auto [end, error] = std::from_chars(first, last, positive);
                error == std::errc{} && end == last) {
                return Json::fromUnsigned(positive);
            }
        }
        double value = 0;
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || end != last || !std::isfinite(value)) {
            fail("a number is out of range");
        }
        return Json::fromDouble(value);
    }

    std::string_view text_;
    std::size_t position_{0};
};

Json Json::array() {
    Json json;
    json.value_ = Array{};
    return json;
}

Json Json::object() {
    Json json;
    json.value_ = Object{};
    return json;
}

Json Json::fromInteger(std::int64_t value) {
    Json json;
    json.value_ = value;
    return json;
}

Json Json::fromUnsigned(std::uint64_t value) {
    // One form for each value, so that equal numbers compare equal however they were made.
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fromInteger(static_cast<std::int64_t>(value));
    }
    Json json;
    json.value_ = value;
    return json;
}

Json Json::fromDouble(double value) {
    Json json;
    if (std::isfinite(value)) {
        json.value_ = value;
    }
    return json;
}

Json Json::parse(std::string_view text) {
    return JsonParser{text}.parseDocument();
}

Result<Json> Json::tryParse(std::string_view text) {
    try {
        return parse(text);
    } catch (const JsonError& error) {
        return Error{ErrorCode::ParseError, error.what()};
    }
}

namespace {

void dumpValue(const Json& json, std::string& out);

void dumpNumber(const Json& json, std::string& out) {
    char buffer[32];
    if (json.isInteger()) {
        // Values above the largest int64_t are only held unsigned.
        const auto [end, error] =
            json.asDouble() < 0 ? std::to_chars(buffer, buffer + sizeof(buffer), json.asInt64())
                                : std::to_chars(buffer, buffer + sizeof(buffer), json.asUint64());
        static_cast<void>(error);
        out.append(buffer, end);
        return;
    }
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), json.asDouble());
    static_cast<void>(error);
    const std::string_view text{buffer, static_cast<std::size_t>(end - buffer)};
    out += text;
    // Keeps 1.0 a fraction, so that reading it back gives the same kind of number.
    if (text.find_first_of(".e") == std::string_view::npos) {
        out += ".0";
    }
}

void dumpValue(const Json& json, std::string& out) {
    switch (json.type()) {
        case Json::Type::Null:
            out += "null";
            return;
        case Json::Type::Bool:
            out += json.asBool() ? "true" : "false";
            return;
        case Json::Type::Number:
            dumpNumber(json, out);
            return;
        case Json::Type::String:
            dumpString(json.asString(), out);
            return;
        case Json::Type::Array: {
            out += '[';
            bool first = true;
            for (const Json& item : json.items()) {
                out += first ? "" : ",";
                first = false;
                dumpValue(item, out);
            }
            out += ']';
            return;
        }
        case Json::Type::Object: {
            out += '{';
            bool first = true;
            for (const JsonMember& member : json.members()) {
                out += first ? "" : ",";
                first = false;
                dumpString(member.key, out);
                out += ':';
                dumpValue(member.value, out);
            }
            out += '}';
            return;
        }
    }
}

}  // namespace

std::string Json::dump() const {
    std::string out;
    dumpValue(*this, out);
    return out;
}

Json::Type Json::type() const noexcept {
    switch (value_.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2:
        case 3:
        case 4: return Type::Number;
        case 5: return Type::String;
        case 6: return Type::Array;
        default: return Type::Object;
    }
}

bool Json::isInteger() const noexcept {
    return std::holds_alternative<std::int64_t>(value_) ||
           std::holds_alternative<std::uint64_t>(value_);
}

bool Json::asBool() const {
    if (const bool* value = std::get_if<bool>(&value_)) {
        return *value;
    }
    wrongType(Type::Bool, type());
}

double Json::asDouble() const {
    if (const auto* value = std::get_if<double>(&value_)) {
        return *value;
    }
    if (const auto* value = std::get_if<std::int64_t>(&value_)) {
        return static_cast<double>(*value);
    }
    if (const auto* value = std::get_if<std::uint64_t>(&value_)) {
        return static_cast<double>(*value);
    }
    wrongType(Type::Number, type());
}

std::int64_t Json::asInt64() const {
    if (const auto* value = std::get_if<std::int64_t>(&value_)) {
        return *value;
    }
    if (std::holds_alternative<std::uint64_t>(value_)) {
        throw JsonError("number out of range");
    }
    if (std::holds_alternative<double>(value_)) {
        throw JsonError("expected a whole number");
    }
    wrongType(Type::Number, type());
}

std::uint64_t Json::asUint64() const {
    if (const auto* value = std::get_if<std::uint64_t>(&value_)) {
        return *value;
    }
    if (const auto* value = std::get_if<std::int64_t>(&value_)) {
        if (*value < 0) {
            throw JsonError("number out of range");
        }
        return static_cast<std::uint64_t>(*value);
    }
    if (std::holds_alternative<double>(value_)) {
        throw JsonError("expected a whole number");
    }
    wrongType(Type::Number, type());
}

const std::string& Json::asString() const {
    if (const auto* value = std::get_if<std::string>(&value_)) {
        return *value;
    }
    wrongType(Type::String, type());
}

const Json::Array& Json::items() const {
    if (const auto* value = std::get_if<Array>(&value_)) {
        return *value;
    }
    wrongType(Type::Array, type());
}

const Json::Object& Json::members() const {
    if (const auto* value = std::get_if<Object>(&value_)) {
        return *value;
    }
    wrongType(Type::Object, type());
}

std::size_t Json::size() const noexcept {
    if (const auto* items = std::get_if<Array>(&value_)) {
        return items->size();
    }
    if (const auto* members = std::get_if<Object>(&value_)) {
        return members->size();
    }
    return 0;
}

const Json* Json::find(std::string_view key) const noexcept {
    const auto* members = std::get_if<Object>(&value_);
    if (members == nullptr) {
        return nullptr;
    }
    for (const JsonMember& member : *members) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

const Json& Json::at(std::string_view key) const {
    if (const Json* value = find(key)) {
        return *value;
    }
    if (!isObject()) {
        wrongType(Type::Object, type());
    }
    throw JsonError("missing field '" + std::string{key} + "'");
}

const Json& Json::at(std::size_t index) const {
    const Array& array = items();
    if (index >= array.size()) {
        throw JsonError("index " + std::to_string(index) + " is out of range");
    }
    return array[index];
}

Json& Json::operator[](std::string_view key) {
    if (isNull()) {
        value_ = Object{};
    }
    auto* members = std::get_if<Object>(&value_);
    if (members == nullptr) {
        wrongType(Type::Object, type());
    }
    for (JsonMember& member : *members) {
        if (member.key == key) {
            return member.value;
        }
    }
    members->push_back(JsonMember{std::string{key}, Json{}});
    return members->back().value;
}

void Json::push(Json item) {
    if (isNull()) {
        value_ = Array{};
    }
    auto* items = std::get_if<Array>(&value_);
    if (items == nullptr) {
        wrongType(Type::Array, type());
    }
    items->push_back(std::move(item));
}

bool operator==(const Json& left, const Json& right) {
    if (left.type() != right.type()) {
        return false;
    }
    switch (left.type()) {
        case Json::Type::Null:
            return true;
        case Json::Type::Bool:
            return left.asBool() == right.asBool();
        case Json::Type::Number:
            if (left.isInteger() && right.isInteger()) {
                const bool negative = left.asDouble() < 0;
                if (negative != (right.asDouble() < 0)) {
                    return false;
                }
                return negative ? left.asInt64() == right.asInt64()
                                : left.asUint64() == right.asUint64();
            }
            return left.asDouble() == right.asDouble();
        case Json::Type::String:
            return left.asString() == right.asString();
        case Json::Type::Array:
            return left.items() == right.items();
        case Json::Type::Object:
            return left.size() == right.size() &&
                   std::all_of(left.members().begin(), left.members().end(),
                               [&](const JsonMember& member) {
                                   const Json* other = right.find(member.key);
                                   return other != nullptr && *other == member.value;
                               });
    }
    return false;
}

}  // namespace cppboot
